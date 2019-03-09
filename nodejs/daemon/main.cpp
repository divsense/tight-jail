#include <v8.h>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <algorithm>
#include <utility>
#include <functional>
#include <atomic>
#include <type_traits>
#include <exception>
#include <limits>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "almost_json_parser.hpp"
#include "uvwrap.hpp"
#include "task_loop.hpp"


#if V8_MAJOR_VERSION >= 7
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length(isolate))
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(isolate,data))
#  define STRING_WRITEONEBYTE(str,isolate,...) ((str)->WriteOneByte(isolate,__VA_ARGS__))
#else
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length())
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(data))
#  define STRING_WRITEONEBYTE(str,isolate,...) ((str)->WriteOneByte(__VA_ARGS__))
#endif

/* This is mostly for errors that would only be caused by the client doing
something it isn't supposed to, where detailed error messages aren't needed
unless debugging. */
#ifdef NDEBUG
#  define DEBUG_MESSAGE(...)
#else
#  define DEBUG_MESSAGE(...) fprintf(stderr,__VA_ARGS__)
#endif

#define RESOLVE_MODULE_FUNC_NAME "__resolveModule"
#define RESOLVE_READY_MODULE_FUNC_NAME "__resolveReadyModule"
#define FAIL_MODULE_FUNC_NAME "__failModule"

/* There are two levels of module cache. Each context has a module instance
cache, but there is also a global cache that contains modules that have been
compiled but not instantiated. */
const char startup_code[] = R"(
var console = {
    log: __console_log
};

class JailImportError extends Error {}
JailImportError.prototype.name = 'JailImportError';

const __moduleCacheByName = {};
const __moduleCacheById = {};

class __PendingMod {
  constructor(resolve1,reject1) {
    this.callbacks = [[resolve1,reject1]];
    this.aliases = [];
  }

  add(resolve,reject) {
    this.callbacks.push([resolve,reject]);
  }

  mergeFrom(b,name) {
    for(let c of b.callbacks) this.callbacks.push(c);
    for(let a of b.aliases) this.aliases.push(a);
    this.aliases.push(name);
  }

  resolve(x,name) {
    __moduleCacheByName[name] = x;
    for(let a of this.aliases) __moduleCacheByName[a] = x;
    for(let c of this.callbacks) c[0](x);
  }

  reject(x,name) {
    delete __moduleCacheByName[name];
    for(let a of this.aliases) delete __moduleCacheByName[a];
    for(let c of this.callbacks) c[1](x);
  }
}

function jimport(m) {
  return new Promise((resolve,reject) => {
    let r = __moduleCacheByName[m];
    if(r === null) reject(new JailImportError('module not found'));
    else if(r instanceof __PendingMod) r.add([resolve,reject]);
    else if(r === undefined) {
      __moduleCacheByName[m] = new __PendingMod(resolve,reject);
      __sendImportMessage(m);
    }
    else resolve(r);
  });
}

function __resolveModule(name,mid,getCachedGlobal) {
  let pending = __moduleCacheByName[name];
  let r = __moduleCacheById[mid];
  if(r instanceof __PendingMod) {
    if(pending !== r) {
      r.mergeFrom(pending,name);
      __moduleCacheByName[name] = r;
      pending.aliases.forEach(item => { __moduleCacheByName[item] = r; });
    }
  } else if(r === undefined) {
    try {
      r = getCachedGlobal(name);
    } catch(e) {
      pending.reject(new JailImportError(e.toString()));
      return;
    }

    if(r === null) {
      /* The module is being compiled. This function or __resolveReadyModule
      will be called when the module is ready. */
      __moduleCacheById[mid] = pending;
    } else {
      __moduleCacheById[mid] = r;
      pending.resolve(r,name);
    }
  } else {
    pending.resolve(r,name);
  }
}

function __resolveReadyModule(name,mid,mod) {
  let pending = __moduleCacheByName[name];
  __moduleCacheById[mid] = mod;
  pending.resolve(mod,name);
}

function __failModule(name,mid,message) {
  if(mid !== null) delete __moduleCacheById[mid];
  __moduleCacheByName[name].reject(new JailImportError(message.toString()),name);
}
)";


const std::string str_type = "type";
const std::string str_code = "code";
const std::string str_func = "func";
const std::string str_args = "args";
const std::string str_context = "context";
const std::string str_connection = "connection";
const std::string str_name = "name";
const std::string str_id = "id";
const std::string str_ids = "ids";
const std::string str_modtype = "modtype";
const std::string str_msg = "msg";
const std::string str_value = "value";
const std::string str_async = "async";


typedef unsigned long long connection_id_t;

struct program_t;
program_t *program;
#ifndef NDEBUG
uv_thread_t _master_thread;
#endif


template<size_t N> char *copy_string(const char (&str)[N]) {
    char *r = new char[N];
    memcpy(r,str,N);
    return r;
}

[[ noreturn ]] void irrecoverable_failure(std::exception &e) {
    /* Rather than risk having a situation where any thread that tries
    importing a specific module, gets soft-locked, it's better to just
    terminate this process. */
    fprintf(stderr,"irrecoverable failure: %s\n",e.what());
    std::terminate();
}

void queue_response(uv_stream_t *stream,char *msg,ssize_t len=-1);

struct js_cache {
    std::string code;
    std::unique_ptr<uint8_t[]> data;
    int length;
};
struct waiting_mod_req {
    std::string name;
    connection_id_t cc_id;
    unsigned long context_id;

    template<typename T> waiting_mod_req(T &&name,connection_id_t cc_id,unsigned long context_id) :
        name{std::forward<T>(name)}, cc_id{cc_id}, context_id{context_id} {}
};
struct uncompiled_buffer {
    std::unique_ptr<char[]> data;
    int length;

    /* if "started" is not zero, then the thread associated with the given
    connection is already compiling the module and other threads will add an
    entry to "waiting" if they need the same module.
    */
    connection_id_t started;
    std::vector<waiting_mod_req> waiting;
};
struct cached_module {
    enum tag_t {UNCOMPILED_WASM_BASE64,UNCOMPILED_WASM,UNCOMPILED_JS,WASM,JS} tag;
    union data_t {
        v8::WasmCompiledModule::TransferrableModule wasm;
        js_cache js;
        uncompiled_buffer buff;

        data_t() {}
        ~data_t() {}
    } data;

    cached_module(v8::WasmCompiledModule::TransferrableModule &&wasm) : tag{WASM} {
        new(&data.wasm) v8::WasmCompiledModule::TransferrableModule(std::move(wasm));
    }

    cached_module(std::string &&code,const uint8_t *compile_data,int length) : tag{JS} {
        new(&data.js) js_cache({std::move(code),std::unique_ptr<uint8_t[]>{new uint8_t[length]},length});
        memcpy(data.js.data.get(),compile_data,length);
    }

    cached_module(const char *mod_data,int length,tag_t t) : tag{t} {
        assert(is_uncompiled());
        new(&data.buff) uncompiled_buffer({std::unique_ptr<char[]>{new char[length]},length,false,{}});
        memcpy(data.buff.data.get(),mod_data,length);
    }

    cached_module(std::unique_ptr<char[]> &&mod_data,int length,tag_t t) : tag{t} {
        assert(is_uncompiled());
        new(&data.buff) uncompiled_buffer({std::move(mod_data),length,false,{}});
    }

    cached_module(cached_module &&b) {
        move_assign(std::move(b));
    }

    ~cached_module() { clear(); }

    cached_module &operator=(cached_module &&b) {
        clear();
        move_assign(std::move(b));
        return *this;
    }

    bool is_uncompiled() const {
        return tag != WASM && tag != JS;
    }

private:
    void clear() {
        switch(tag) {
        case WASM:
            data.wasm.~TransferrableModule();
            break;
        case JS:
            data.js.~js_cache();
            break;
        default:
            assert(is_uncompiled());
            data.buff.~uncompiled_buffer();
            break;
        }
    }

    void move_assign(cached_module &&b) {
        tag = b.tag;
        switch(tag) {
        case WASM:
            new(&data.wasm) v8::WasmCompiledModule::TransferrableModule(std::move(b.data.wasm));
            break;
        case JS:
            new(&data.js) js_cache(std::move(b.data.js));
            break;
        default:
            assert(is_uncompiled());
            new(&data.buff) uncompiled_buffer(std::move(b.data.buff));
            break;
        }
    }
};

/* a seperate struct so that "Dispose()" and "ShutdownPlatform()" get called
even if the constructor for program_t throws */
struct v8_program_t {
    v8_platform platform;

    v8_program_t() {
        v8::V8::InitializePlatform(&platform);
        v8::V8::Initialize();
    }

    ~v8_program_t() {
        v8::V8::Dispose();
        v8::V8::ShutdownPlatform();
    }
};

struct command_dispatcher {
    almost_json_parser::object_parser command_buffer;

    /* when true, there was a parse error in current request and further input
    will be ignored until the start of the next request */
    bool parse_error;

    command_dispatcher() : parse_error{false} {}

    void handle_input(uv_stream_t *output_s,ssize_t read,const uv_buf_t *buf) noexcept;
    virtual void register_task() = 0;
    virtual void on_read_error() = 0;
    virtual void on_alloc_error() {
        fprintf(stderr,"insufficient memory for command");
    }
    int type_str_to_index(uv_stream_t *output_s,int size,const std::string *types);

protected:
    ~command_dispatcher() = default;
};

void command_dispatcher::handle_input(uv_stream_t *output_s,ssize_t read,const uv_buf_t *buf) noexcept {
    if(read < 0) {
        on_read_error();
    } else if(read > 0) {
        try {
            auto start = buf->base;
            auto end = start + read;
            auto zero = std::find(start,end,0);

            for(;;) {
                try {
                    while(zero != end) {
                        if(!parse_error) {
                            command_buffer.feed(start,zero-start);
                            command_buffer.finish();
                            register_task();
                        }
                        command_buffer.reset();
                        parse_error = false;

                        start = zero + 1;
                        zero = std::find(start,end,0);
                    }

                    if(!parse_error) command_buffer.feed(start,end-start);
                    break;
                } catch(almost_json_parser::syntax_error&) {
                    parse_error = true;
                    queue_response(
                        output_s,
                        copy_string(R"({"type":"error","errtype":"request","message":"invalid JSON"})"));
                }
            }
        } catch(std::bad_alloc&) {
            on_alloc_error();
        }
    }

    delete[] buf->base;
}

/* prevents accidentally accessing data without locking a mutex */
template<typename T> class thread_shared_data {
    T _data;

public:
    template<typename... U> thread_shared_data(U&&... args) : _data{std::forward<U>(args)...} {}
    thread_shared_data(const thread_shared_data &b) = delete;

    thread_shared_data &operator=(const thread_shared_data &b) = delete;

    T *get(uvwrap::mutex_scope_lock&) { return &_data; }
    const T *get(uvwrap::mutex_scope_lock&) const { return &_data; }
};

struct module_cache_item {
    const std::string id;
    thread_shared_data<cached_module> mod;

    template<typename T,typename... U> module_cache_item(T &&id,U&&... args) :
        id{std::forward<T>(id)}, mod{std::forward<U>(args)...} {}

    cached_module *get(uvwrap::mutex_scope_lock &lock) { return mod.get(lock); }
    const cached_module *get(uvwrap::mutex_scope_lock &lock) const { return mod.get(lock); }
};

bool operator<(const std::shared_ptr<module_cache_item> &a,const std::shared_ptr<module_cache_item> &b) {
    return a->id < b->id;
}
bool operator<(const std::shared_ptr<module_cache_item> &a,const std::string &b) {
    return a->id < b;
}
bool operator<(const std::string &a,const std::shared_ptr<module_cache_item> &b) {
    return a < b->id;
}

struct client_connection;

struct program_t : v8_program_t, command_dispatcher {
    friend struct client_connection;

    /* this is technically not a map, but by using the template form of "find",
    we treat it like one */
    typedef std::set<std::shared_ptr<module_cache_item>,std::less<>> module_cache_map;

    typedef std::map<connection_id_t,client_connection*> connection_map;

    uvwrap::mutex_t module_cache_lock;
    uvwrap::loop_t main_loop;
    uvwrap::pipe_t_auto server;
    uvwrap::signal_t sig_request;
    uvwrap::pipe_t_auto stdin;
    uvwrap::pipe_t_auto stdout;
    v8::StartupData snapshot;
    v8::Isolate::CreateParams create_params;
    module_cache_map module_cache;
    connection_id_t next_conn_index;
    bool shutting_down;
    bool console_enabled;

    program_t();
    ~program_t();

    void on_read_error() override {
        fprintf(stderr,"error in standard-in\n");
    }

    void register_task() override;

private:
    uvwrap::mutex_t func_dispatch_lock;
    uvwrap::async_t func_dispatcher;
    std::vector<std::function<void()>> func_requests;

    uvwrap::rwlock_t connections_lock;
    connection_map connections;

public:
    void run_on_master_thread(std::function<void()> f) {
        {
            uvwrap::mutex_scope_lock lock{func_dispatch_lock};
            func_requests.push_back(std::move(f));
        }
        func_dispatcher.send();
    }

    void run_functions() {
        std::vector<std::function<void()>> tmp;
        {
            uvwrap::mutex_scope_lock lock{func_dispatch_lock};
            std::swap(tmp,func_requests);
        }
        for(auto &f : tmp) f();
    }

    client_connection *get_connection(connection_id_t id) {
        uvwrap::read_scope_lock lock{connections_lock};
        auto itr = connections.find(id);
        return itr == connections.end() ? nullptr : itr->second;
    }

    size_t connection_count() {
        uvwrap::read_scope_lock lock{connections_lock};
        return connections.size();
    }

    /* if a client connection started compiling one or more modules, but is
    terminated before it could finish, it might not be able to signal to the
    other threads waiting for the module (this can happen if the connection was
    waiting for a JS Promise to compile the module). This function will scan for
    modules in the cache, that were started by a no longer existing connection.
    */
    void audit_module_cache() noexcept;

    size_t cache_count() {
        uvwrap::mutex_scope_lock lock{module_cache_lock};
        return module_cache.size();
    }
};

struct bad_request_js {};
struct bad_context {};

template<typename T> v8::Local<T> check_r(v8::Local<T> x) {
    if(x.IsEmpty()) throw bad_request_js{};
    return x;
}

template<typename T> v8::Local<T> check_r(v8::MaybeLocal<T> x) {
    v8::Local<T> r;
    if(!x.ToLocal(&r)) throw bad_request_js{};
    return r;
}

template<typename T> v8::Local<T> check_r(v8::Maybe<T> x) {
    T r;
    if(!x.To(&r)) throw bad_request_js{};
    return r;
}

std::string to_std_str(v8::Isolate *isolate,v8::Local<v8::Value> x) {
    v8::String::Utf8Value id{isolate,x};
    if(!*id) throw bad_request_js{};
    return std::string{*id,size_t(id.length())};
}

v8::Local<v8::String> from_std_str(v8::Isolate *isolate,const std::string &x) {
    return check_r(v8::String::NewFromUtf8(
        isolate,
        x.data(),
        v8::NewStringType::kNormal,
        x.size()));
}

void throw_js_error(v8::Isolate *isolate,const char *msg) {
    isolate->ThrowException(
        v8::Exception::Error(
            v8::String::NewFromUtf8(
                isolate,
                msg,
                v8::NewStringType::kNormal).ToLocalChecked()));
}

void throw_js_typeerror(v8::Isolate *isolate,const char *msg) {
    isolate->ThrowException(
        v8::Exception::TypeError(
            v8::String::NewFromUtf8(
                isolate,
                msg,
                v8::NewStringType::kNormal).ToLocalChecked()));
}


void sendImportMessage(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept;
void console_log(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept;
const intptr_t external_references[] = {
    reinterpret_cast<intptr_t>(&sendImportMessage),
    reinterpret_cast<intptr_t>(&console_log),
    0};


struct client_task;


/* a seperate struct so that "isolate->Dispose()" gets called even if the
constructor for client_connection throws */
struct autoclose_isolate {
    v8::Isolate* isolate;

    autoclose_isolate(v8::Isolate* isolate) : isolate{isolate} {
        isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    }

protected:
    ~autoclose_isolate() { isolate->Dispose(); }
};

struct client_connection final : public autoclose_isolate, command_dispatcher {
    typedef std::map<unsigned long,v8::UniquePersistent<v8::Context>> context_map;
    typedef std::pair<std::shared_ptr<module_cache_item>,v8::UniquePersistent<v8::External>> cmod_list_data;
    typedef std::list<cmod_list_data> cmod_list;
    typedef std::pair<client_connection*,cmod_list::iterator> cmod_list_ref;

    static constexpr int CONTEXT_ID_INDEX = 0;

    uvwrap::pipe_t_manual client;

    std::shared_ptr<task_loop> tloop;
    v8::UniquePersistent<v8::Context> request_context;
    context_map contexts;

    /* When cached modules need to be passed across JavaScript functions, we
    can't rely on the javascript functions to actually get called, since the
    interpreter can be terminated at any time, so weak references are stored
    here. */
    cmod_list transient_cmods;

    unsigned long next_id;
    connection_id_t connection_id;
    uint32_t next_async_id;

    v8::UniquePersistent<v8::ObjectTemplate> globals_template;
    v8::UniquePersistent<v8::ObjectTemplate> mod_globals_template;

    int compiling;

    volatile bool closed;

    enum {
        STR_STRING=0,
        RESOLVE_MODULE,
        RESOLVE_READY_MODULE,
        FAIL_MODULE,
        PROMISE,
        RESOLVE,
        EXPORTS,
        WEBASSEMBLY,
        COMPILE,
        INSTANTIATE,
        COUNT_STR
    };

    v8::UniquePersistent<v8::String> str[COUNT_STR];
    static const char *str_raw[COUNT_STR];

    client_connection() :
            autoclose_isolate{v8::Isolate::New(program->create_params)},
            client{program->main_loop,this},
            tloop{program->platform.get_task_loop(isolate)},
            next_id{1},
            next_async_id{0},
            compiling{0},
            closed{false}
    {
        isolate->SetData(0,this);

        uvwrap::write_scope_lock lock{program->connections_lock};
        connection_id = program->next_conn_index++;
        program->connections.emplace(connection_id,this);
    }

    ~client_connection() {
        assert(closed);

        /* all the JS references need to be freed before calling
           isolate->Dispose */
        for(auto &item : str) item.Reset();
        for(auto &item : transient_cmods) {
            // make sure the weak callback is not called
            delete item.second.ClearWeak<cmod_list_ref>();

            item.second.Reset();
        }
        request_context.Reset();
        globals_template.Reset();
        contexts.clear();
        program->platform.remove_task_loop(isolate);
    }

    std::pair<unsigned long,v8::Local<v8::Context>> new_user_context(bool weak);

    v8::MaybeLocal<v8::String> try_get_str(unsigned int i) noexcept;
    v8::Local<v8::String> get_str(unsigned int i) { return check_r(try_get_str(i)); }

    void close();

    void close_async() noexcept;

    void register_task() override;

    static void close_all() {
        for(auto &pair : program->connections) pair.second->close();
    }

    void on_read_error() override {
        close();
    }
    void on_alloc_error() override {
        command_dispatcher::on_alloc_error();
        close();
    }

    template<typename TaskT,typename ...T> void register_other_task(T&&... x) {
        tloop->PostTask(std::unique_ptr<TaskT>(new TaskT(this,std::forward<T>(x)...)));
    }

    static unsigned long get_context_id(v8::Local<v8::Context> c) {
        return reinterpret_cast<uintptr_t>(
            v8::External::Cast(*c->GetEmbedderData(CONTEXT_ID_INDEX))->Value());
    }

    v8::Local<v8::External> new_cmod_node(std::shared_ptr<module_cache_item> cmod) {
        transient_cmods.push_front(cmod_list_data{cmod,v8::UniquePersistent<v8::External>{}});
        auto &item = transient_cmods.front();
        auto e = v8::External::New(isolate,&item);
        item.second.Reset(isolate,e);
        item.second.SetWeak(
            new cmod_list_ref(this,transient_cmods.begin()),
            [](const v8::WeakCallbackInfo<cmod_list_ref> &data) {
                auto ref = data.GetParameter();
                ref->first->transient_cmods.erase(ref->second);
                delete ref;
            },
            v8::WeakCallbackType::kParameter);

        return e;
    }
};


v8::Local<v8::ObjectTemplate> get_globals(v8::Isolate *isolate) {
    auto ft = v8::ObjectTemplate::New(isolate);

    ft->Set(
        check_r(v8::String::NewFromUtf8(
            isolate,
            "__sendImportMessage",
            v8::NewStringType::kNormal)),
        v8::FunctionTemplate::New(isolate,&sendImportMessage));

    ft->Set(
        check_r(v8::String::NewFromUtf8(
            isolate,
            "__console_log",
            v8::NewStringType::kNormal)),
        v8::FunctionTemplate::New(isolate,&console_log));

    return ft;
}

std::pair<unsigned long,v8::Local<v8::Context>> client_connection::new_user_context(bool weak) {
    typedef std::pair<client_connection*,context_map::iterator> map_ref;

    unsigned long id = next_id++;
    auto c = v8::Context::New(isolate);
    c->SetEmbedderData(
        CONTEXT_ID_INDEX,
        v8::External::New(isolate,reinterpret_cast<void*>(uintptr_t(id))));
    auto itr = contexts.emplace(id,v8::UniquePersistent<v8::Context>{isolate,c}).first;
    if(weak) itr->second.SetWeak(
        new map_ref(this,itr),
        [](const v8::WeakCallbackInfo<map_ref> &data) {
            auto ref = data.GetParameter();
            ref->first->contexts.erase(ref->second);
            delete ref;
        },
        v8::WeakCallbackType::kParameter);
    return std::make_pair(id,c);
}

v8::MaybeLocal<v8::String> client_connection::try_get_str(unsigned int i) noexcept {
    assert(i < COUNT_STR);
    if(str[i].IsEmpty()) {
        v8::Local<v8::String> tmp;
        if(!v8::String::NewFromUtf8(
            isolate,
            str_raw[i],
            v8::NewStringType::kNormal).ToLocal(&tmp)) return {};
        str[i].Reset(isolate,tmp);
        return tmp;
    }
    return str[i].Get(isolate);
}

const char *client_connection::str_raw[client_connection::COUNT_STR] = {
    "string",
    RESOLVE_MODULE_FUNC_NAME,
    RESOLVE_READY_MODULE_FUNC_NAME,
    FAIL_MODULE_FUNC_NAME,
    "Promise",
    "resolve",
    "exports",
    "WebAssembly",
    "compile",
    "instantiate"
};

void client_connection::close_async() noexcept {
    try {
        program->run_on_master_thread([=] { this->close(); });
    } catch(std::exception &e) {
        irrecoverable_failure(e);
    }
}

struct write_request {
    uv_write_t req;
    std::unique_ptr<char[]> msg;

    write_request(char *msg) : msg(msg) {
        req.data = this;
    }
};

struct client_task : public v8::Task {
    friend struct client_connection;

    client_connection *client;
    bool cancelled;

    void cancel() {
        cancelled = true;
    }
protected:
    client_task(client_connection *client) : client{client}, cancelled{false} {}
};

struct request_task : client_task {
    friend struct client_connection;

    enum type_t {
        CREATE_CONTEXT = 0,
        DESTROY_CONTEXT,
        EVAL,
        EXEC,
        CALL,
        GETSTATS,
        COUNT_TYPES
    } type;

    static const std::string request_types[COUNT_TYPES];

    almost_json_parser::value_map values;

    void Run() override;

    v8::Local<v8::Value> get_value(const std::string &name,v8::Local<v8::Context> context) const;
    std::pair<unsigned long,v8::Local<v8::Context>> get_context(v8::Local<v8::Context> request_context);

private:
    request_task(client_connection *client,type_t type) :
        client_task{client},
        type{type} {}
};

enum module_req_type_t {
    MODULE = 0,
    MODULE_CACHED,
    MODULE_ERROR,
    PURGE_CACHE,
    MODULE_REQ_COUNT_TYPES
};

const std::string module_request_types[MODULE_REQ_COUNT_TYPES] = {
    "module",
    "modulecached",
    "moduleerror",
    "purgecache"
};

struct module_task : client_task {
    std::string name;
    unsigned long context_id;
    std::shared_ptr<module_cache_item> cmod;

    void Run() override;

    template<typename T>
    module_task(client_connection *cc,T &&name,unsigned long context_id,const std::shared_ptr<module_cache_item> &cmod) :
        client_task{cc},
        name{std::forward<T>(name)},
        context_id{context_id},
        cmod{cmod} {}
};

struct module_error_task : client_task {
    std::string name;
    std::string mid;
    std::string msg;
    unsigned long context_id;
    void Run() override;

    template<typename T1,typename T2,typename T3>
    module_error_task(client_connection *cc,T1 &&name,T2 &&mid,T3 &&msg,unsigned long context_id) :
        client_task{cc},
        name{std::forward<T1>(name)},
        mid{std::forward<T2>(mid)},
        msg{std::forward<T3>(msg)},
        context_id{context_id} {}
};

const std::string request_task::request_types[request_task::COUNT_TYPES] = {
    "createcontext",
    "destroycontext",
    "eval",
    "exec",
    "call",
    "getstats"
};

v8::Local<v8::Value> to_value(
    v8::Isolate *isolate,
    v8::Local<v8::Context> context,
    const almost_json_parser::parsed_value &val)
{
    auto r = check_r(v8::String::NewFromUtf8(
        isolate,
        val.data.data(),
        v8::NewStringType::kNormal,
        val.data.size()));
    return val.is_string ? static_cast<v8::Local<v8::Value>>(r) : check_r(v8::JSON::Parse(context,r));
}

v8::Local<v8::Value> request_task::get_value(const std::string &name,v8::Local<v8::Context> context) const {
    auto itr = values.find(name);
    if(itr == values.end()) {
        DEBUG_MESSAGE("missing attribute \"%s\"\n",name.c_str());
        throw_js_error(client->isolate,"invalid request");
        throw bad_request_js{};
    }

    return to_value(client->isolate,context,itr->second);
}

void queue_response(uv_stream_t *stream,char *msg,ssize_t len) {
    if(len < 0) len = strlen(msg);
    auto req = new write_request(msg);
    char zero[1] = {0};
    uv_buf_t buf[2] = {uv_buf_init(msg,static_cast<unsigned int>(len)),uv_buf_init(zero,1)};
    uv_write(
        &req->req,
        stream,
        buf,
        2,
        [](uv_write_t *req,int status) {
            delete reinterpret_cast<write_request*>(req->data);
            if(status < 0) {
                if(status != UV_ECANCELED)
                    fprintf(stderr,"error writing output: %s\n",uv_strerror(status));
            }
        });
}

void queue_response(uvwrap::pipe_t &stream,char *msg,ssize_t len=-1) {
    queue_response(
        stream.as_uv_stream(),
        msg,
        len);
}

void client_connection::register_task() {
    int rtype = type_str_to_index(
        client.as_uv_stream(),
        request_task::COUNT_TYPES,
        request_task::request_types);
    if(rtype == -1) return;

    auto task = std::unique_ptr<request_task>(new request_task(this,static_cast<request_task::type_t>(rtype)));
    std::swap(task->values,command_buffer.value);
    tloop->PostTask(std::move(task));
}

void client_connection::close() {
#ifndef NDEBUG
    auto _tmp = uv_thread_self();
    assert(uv_thread_equal(&_master_thread,&_tmp));
#endif

    if(!closed) {
        closed = true;
        {
            uvwrap::write_scope_lock lock{program->connections_lock};
            program->connections.erase(connection_id);
        }
        isolate->TerminateExecution();
        tloop->close();
        if(compiling) program->audit_module_cache();

        client.close(
            [](uv_handle_t *h) {
                auto self = reinterpret_cast<client_connection*>(h->data);
                delete self;
            });
    }
}

int command_dispatcher::type_str_to_index(uv_stream_t *output_s,int size,const std::string *types) {
    auto type = command_buffer.value.find(str_type);
    if(type == command_buffer.value.end() || !type->second.is_string) {
        DEBUG_MESSAGE(
            type == command_buffer.value.end() ?
            "missing attribute \"type\"\n" :
            "\"type\" is not a string\n");
        queue_response(
            output_s,
            copy_string(R"({"type":"error","errtype":"request","message":"invalid request"})"));
        return -1;
    }

    for(int i=0; i<size; ++i) {
        if(types[i] == type->second.data) {
            return i;
        }
    }

    queue_response(
        output_s,
        copy_string(R"({"type":"error","errtype":"request","message":"unknown request type"})"));
    return -1;
}

char hex_digit(int x) {
    assert(x >= 0 && x < 16);
    return "01234567890abcdef"[x];
}

class string_builder {
    std::vector<char> buffer;

    void add_escaped_char(uint16_t c);
    void add_escaped_char(uint8_t c);
    template<typename T> void add_integer_s(T i);
    template<typename T> void add_integer_u(T i);

public:
    string_builder(size_t reserve=200) {
        buffer.reserve(reserve);
    }

    void reserve_more(size_t amount) {
        buffer.reserve(buffer.size() + amount);
    }

    char *get_string() const;

    void add_char(char x) {
        buffer.push_back(x);
    }

    void add_escaped(const v8::String::Value &str);
    void add_escaped(const std::u16string &str);
    void add_escaped_ascii(const char *str);

    void add_string(const char *str,size_t length) {
        buffer.insert(buffer.end(),str,str+length);
    }

    template<size_t N> void add_string(const char (&str)[N]) {
        add_string(str,N-1);
    }

    void add_string(v8::Isolate* isolate,const v8::String *str) {
        size_t length = buffer.size();
        buffer.insert(buffer.end(),STRING_UTF8LENGTH(str,isolate),0);
        STRING_WRITEUTF8(str,isolate,buffer.data() + length);
    }

    void add_integer(long i) { add_integer_s<long>(i); }
    void add_integer(unsigned long i) { add_integer_u<unsigned long>(i); }
    void add_integer(unsigned long long i) { add_integer_u<unsigned long long>(i); }

    void add_integer(int i) { add_integer_s<long>(i); }
    void add_integer(unsigned int i) { add_integer_s<unsigned long>(i); }

    size_t size() const { return buffer.size(); }
};

char *string_builder::get_string() const {
    char *r = new char[buffer.size() + 1];
    memcpy(r,buffer.data(),buffer.size());
    r[buffer.size()] = 0;
    return r;
}

void string_builder::add_escaped(const v8::String::Value &str) {
    reserve_more(str.length());
    for(int i = 0; i < str.length(); i++) add_escaped_char((*str)[i]);
}

void string_builder::add_escaped(const std::u16string &str) {
    reserve_more(str.size());
    for(char16_t c : str) add_escaped_char(uint16_t(c));
}

void string_builder::add_escaped_ascii(const char *str) {
    while(*str) add_escaped_char(uint8_t(*str++));
}

void string_builder::add_escaped_char(uint8_t c) {
    if(c <= 0x7f && isprint(c)) add_char(c);
    else {
        add_char('\\');
        add_char('x');
        add_char(hex_digit(c >> 4));
        add_char(hex_digit(c & 0xf));
    }
}

void string_builder::add_escaped_char(uint16_t c) {
    if(c <= 0xff) {
        add_escaped_char(static_cast<uint8_t>(c));
    } else {
        add_char('\\');
        add_char('u');
        add_char(hex_digit(c >> 12));
        add_char(hex_digit((c >> 8) & 0xf));
        add_char(hex_digit((c >> 4) & 0xf));
        add_char(hex_digit(c & 0xf));
    }
}

template<typename T> void string_builder::add_integer_s(T i) {
    typedef typename std::make_unsigned<T>::type utype;

    if(i == 0) {
        add_char('0');
        return;
    }
    auto start = buffer.end();
    bool neg = false;
    utype j;
    if(i < 0) {
        neg = true;
        j = static_cast<utype>(-(i + 1)) + 1; // negate without overflow
    } else j = i;

    while(j) {
        add_char(hex_digit(j % 10));
        j = j / 10;
    }

    if(neg) add_char('-');

    std::reverse(start,buffer.end());
}

template<typename T> void string_builder::add_integer_u(T i) {
    if(i == 0) {
        add_char('0');
        return;
    }
    auto start = buffer.end();
    while(i) {
        add_char(hex_digit(i % 10));
        i = i / 10;
    }

    std::reverse(start,buffer.end());
}

template<typename T> struct _escaped {
    T &val;
    explicit _escaped(T &val) : val(val) {}
};

_escaped<v8::String::Value> escaped(v8::String::Value &val) {
    return _escaped<v8::String::Value>{val};
}

_escaped<const std::u16string> escaped(const std::u16string &val) {
    return _escaped<const std::u16string>{val};
}

struct response_builder {
    string_builder build;
    v8::Isolate* isolate;

    response_builder(v8::Isolate* isolate) : isolate(isolate) {}

    void send(uv_stream_t *stream) {
        assert(build.size());
        build.add_char('}');
        queue_response(stream,build.get_string());
    }

    void send(uvwrap::pipe_t &stream) { send(stream.as_uv_stream()); }

    void add_name(const char *str,size_t length) {
        build.add_char(build.size() ? ',' : '{');
        build.add_char('"');
        build.add_string(str,length);
        build.add_char('"');
    }

    template<size_t N> void add_part(const char (&value)[N]) {
        build.add_string(value);
    }

    template<typename T> void add_part(T value) {
        build.add_integer(value);
    }

    void add_part(v8::String *value) {
        assert(isolate);
        build.add_string(isolate,value);
    }

    template<size_t N,typename T> response_builder &operator()(const char (&name)[N],const T &value) {
        add_name(name,N-1);
        build.add_char(':');
        add_part(value);

        return *this;
    }

    template<size_t N,typename T> response_builder &operator()(const char (&name)[N],_escaped<T> value) {
        add_name(name,N-1);
        build.add_char(':');
        build.add_char('"');
        build.add_escaped(value.val);
        build.add_char('"');

        return *this;
    }
};

void queue_js_exception(uv_stream_t *stream,v8::TryCatch &tc,v8::Isolate *isolate,v8::Local<v8::Context> context) {
    using namespace v8;

    String::Value exc_str{isolate,tc.Exception()};
    char *msg;
    if(*exc_str) {
        string_builder tmp{static_cast<size_t>(exc_str.length()) + 100};
        tmp.add_string(R"({"type":"error","errtype":"request","message":")");
        tmp.add_escaped(exc_str);
        tmp.add_string("\"}");
        msg = tmp.get_string();
    } else {
        msg = copy_string(R"({"type":"error","errtype":"request","message":null})");
    }
    queue_response(stream,msg);
}

void queue_script_exception(uv_stream_t *stream,v8::Local<v8::Value> exc,v8::Local<v8::Message> exc_msg,v8::Isolate *isolate,v8::Local<v8::Context> context,v8::Maybe<uint32_t> async_id=v8::Nothing<uint32_t>()) {
    using namespace v8;

    response_builder rb{nullptr};
    if(async_id.IsJust()) {
        rb("type","\"asyncresultexception\"");
        rb("id",async_id.FromJust());
    } else {
        rb("type","\"resultexception\"");
    }

    TryCatch tc{isolate};
    String::Value exc_str{isolate,exc};

    if(*exc_str) {
        rb("message",escaped(exc_str));

        if(!exc_msg.IsEmpty()) {
            String::Value filename{isolate,exc_msg->GetScriptResourceName()};
            if(*filename) rb("filename",escaped(filename));
            else rb("filename","null");

            Maybe<int> lineno = exc_msg->GetLineNumber(context);
            if(lineno.IsJust()) rb("lineno",lineno.FromJust());
            else rb("lineno","null");
        } else {
            rb("filename","null");
            rb("lineno","null");
        }
    } else {
        tc.Reset();
        rb("message","\"<failed to convert exception to string>\"");
        rb("filename","null");
        rb("lineno","null");
    }

    rb.send(stream);
}

void queue_script_exception(uv_stream_t *stream,v8::TryCatch &tc,v8::Isolate *isolate,v8::Local<v8::Context> context,v8::Maybe<uint32_t> async_id=v8::Nothing<uint32_t>()) {
    queue_script_exception(stream,tc.Exception(),tc.Message(),isolate,context,async_id);
    tc.Reset();
}

void queue_script_exception(uv_stream_t *stream,v8::Local<v8::Value> exc,v8::Isolate *isolate,v8::Local<v8::Context> context,v8::Maybe<uint32_t> async_id=v8::Nothing<uint32_t>()) {
    queue_script_exception(
        stream,
        exc,

        /* there should to be a way to get a native exception from an instance
        of Value but I don't see one */
        v8::Local<v8::Message>{},

        isolate,
        context,
        async_id);
}

void sendImportMessage(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    if(args.Length() < 1) return;

    auto isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    v8::String::Value mname{isolate,args[0]};
    if(!*mname) return;

    try {
        std::u16string req{reinterpret_cast<char16_t*>(*mname),size_t(mname.length())};

        auto cc_id = cc->connection_id;
        auto con_id = client_connection::get_context_id(context);

        program->run_on_master_thread([=] {
            response_builder rb{nullptr};
            rb("type","\"import\"");
            rb("name",escaped(req));
            rb("context",con_id);
            rb("connection",cc_id);
            rb.send(program->stdout);
        });
    } catch(std::exception &e) { throw_js_error(isolate,e.what()); }
}

void console_log(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    if(!program->console_enabled) return;

    v8::HandleScope scope(args.GetIsolate());

    if(args.Length() < 1) {
        throw_js_typeerror(args.GetIsolate(),"1 argument required");
        return;
    }

    v8::String::Utf8Value msg{args.GetIsolate(),args[0]};
    if(!*msg) return;

    fprintf(stderr,"%s\n",*msg);
}

/* Run a script and return the resulting "exports" object */
struct execute_as_module {
    v8::Local<v8::Context> context;
    v8::Context::Scope cscope;
    v8::Local<v8::String> str_exports;

    execute_as_module(client_connection *cc)
        : context{cc->new_user_context(true).second}, cscope{context}
    {
        if(!cc->try_get_str(client_connection::EXPORTS).ToLocal(&str_exports) ||
            context->Global()->Set(context,str_exports,v8::Object::New(cc->isolate)).IsNothing()) throw bad_request_js{};
    }

    v8::Local<v8::Value> operator()(client_connection *cc,v8::Local<v8::Script> script) {
        bool has;
        if(script->Run(context).IsEmpty() ||
            !context->Global()->Has(context,str_exports).To(&has)) throw bad_request_js {};

        if(has) return check_r(context->Global()->Get(context,str_exports));
        return v8::Object::New(cc->isolate);
    }
};

// this is for when x is supposed to be one of our JS functions
v8::MaybeLocal<v8::Function> to_function(v8::Isolate *isolate,v8::Local<v8::Value> x) {
    if(!x->IsFunction()) {
        throw_js_typeerror(isolate,"core function missing");
        return {};
    }
    return v8::Local<v8::Function>::Cast(x);
}

std::pair<std::unique_ptr<char[]>,size_t> base64_decode(const char *data,size_t size) {
    static constexpr char pad_char = '=';

    if(size % 4) return {};

    size_t pad = 0;
    if(size) {
        if(data[size-1] == pad_char) {
            ++pad;
            if(data[size-2] == pad_char) ++pad;
        }
    }

    size_t result_size = size/4*3 - pad;
    std::unique_ptr<char[]> result{new char[result_size]};

    auto dest = reinterpret_cast<uint8_t*>(result.get());

    unsigned long temp=0;
    const char *end = data + size;
    while(data < end) {
        for(int i = 0; i < 4; i++) {
            temp <<= 6;
            if(*data >= 0x41 && *data <= 0x5A)
                temp |= *data - 0x41;
            else if(*data >= 0x61 && *data <= 0x7A)
                temp |= *data - 0x47;
            else if(*data >= 0x30 && *data <= 0x39)
                temp |= *data + 0x04;
            else if(*data == 0x2B)
                temp |= 0x3E;
            else if(*data == 0x2F)
                temp |= 0x3F;
            else if(*data == pad_char) {
                switch(end - data) {
                case 1:
                    *dest++ = (temp >> 16) & 0xFF;
                    *dest++ = (temp >> 8 ) & 0xFF;
                    goto end;
                case 2:
                    *dest++ = (temp >> 10) & 0xFF;
                    goto end;
                default:
                    return {};
                }
            } else return {};
            ++data;
        }
        *dest++ = (temp >> 16) & 0xFF;
        *dest++ = (temp >> 8) & 0xFF;
        *dest++ = (temp) & 0xFF;
    }

end:
    return {std::move(result),result_size};
}

bool call_webassembly_func(
    client_connection *cc,
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> obj,
    v8::Local<v8::Value> closure_data,
    v8::FunctionCallback on_success,
    v8::FunctionCallback on_fail,
    int func) noexcept
{
    using namespace v8;

    Local<Value> tmp;
    Local<Object> webasm;
    Local<String> webasm_str, compile_str;
    if(!(cc->try_get_str(client_connection::WEBASSEMBLY).ToLocal(&webasm_str) &&
        cc->try_get_str(func).ToLocal(&compile_str) &&
        context->Global()->Get(context,webasm_str).ToLocal(&tmp) &&
        tmp->ToObject(context).ToLocal(&webasm) &&
        webasm->Get(context,compile_str).ToLocal(&tmp))) return false;

    const char *name = client_connection::str_raw[func];

    if(!tmp->IsFunction()) {
        const char fmt[] = "WebAssembly.%s is not a function";
        std::unique_ptr<char[]> msg{new char[sizeof(fmt) + strlen(name)]};
        sprintf(msg.get(),fmt,name);
        throw_js_typeerror(cc->isolate,msg.get());
        return false;
    }
    auto compile_f = Local<Function>::Cast(tmp);
    Local<Value> args_f[] = {obj};
    if(!compile_f->Call(
        context,
        context->Global(),
        sizeof(args_f)/sizeof(Local<Value>),
        args_f).ToLocal(&tmp)) return false;

    if(!tmp->IsPromise()) {
        const char fmt[] = "WebAssembly.%s did not return a promise";
        std::unique_ptr<char[]> msg{new char[sizeof(fmt) + strlen(name)]};
        sprintf(msg.get(),fmt,name);
        throw_js_typeerror(cc->isolate,msg.get());
        return false;
    }

    auto promise = Local<Promise>::Cast(tmp);
    Local<Function> tmp_fthen;
    Local<Function> tmp_fcatch;
    Local<Promise> tmp_p;
    if(!(Function::New(context,on_success,closure_data).ToLocal(&tmp_fthen) &&
        Function::New(context,on_fail,closure_data).ToLocal(&tmp_fcatch) &&
        promise->Then(context,tmp_fthen).ToLocal(&tmp_p)) ||
        tmp_p->Catch(context,tmp_fcatch).IsEmpty()) return false;

    return true;
}

void wasm_instantiate_success(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1) {
        throw_js_typeerror(isolate,"expected one argument");
        return;
    }

    Local<Object> vals;
    Local<Value> name;
    Local<Value> e_node;
    if(!(args.Data()->ToObject(context).ToLocal(&vals) &&
        vals->Get(context,0).ToLocal(&name) &&
        vals->Get(context,1).ToLocal(&e_node))) return;

    auto node = reinterpret_cast<client_connection::cmod_list_data*>(Local<External>::Cast(e_node)->Value());

    Local<String> js_mid;
    Local<String> str_exports;
    Local<Value> exports;
    Local<Object> mod_obj;
    if(!(v8::String::NewFromUtf8(
            isolate,
            node->first->id.data(),
            v8::NewStringType::kNormal,
            node->first->id.size()).ToLocal(&js_mid) &&
        args[0]->ToObject(context).ToLocal(&mod_obj) &&
        cc->try_get_str(client_connection::EXPORTS).ToLocal(&str_exports) &&
        mod_obj->Get(context,str_exports).ToLocal(&exports))) return;
    Local<String> func_name;
    Local<Value> func_val;
    Local<Function> func;
    Local<Value> args_f[] = {name,js_mid,exports};
    if(!(cc->try_get_str(client_connection::RESOLVE_READY_MODULE).ToLocal(&func_name) &&
        context->Global()->Get(context,func_name).ToLocal(&func_val) &&
        to_function(isolate,func_val).ToLocal(&func)) ||
        func->Call(context,
            context->Global(),
            sizeof(args_f)/sizeof(Local<Value>),
            args_f).IsEmpty()) return;
}

bool notify_module_failure(
    client_connection *cc,
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> name,
    const std::string &mid,
    v8::Local<v8::Value> err_obj) noexcept
{
    using namespace v8;

    Local<String> js_mid;
    if(!String::NewFromUtf8(
        cc->isolate,
        mid.data(),
        NewStringType::kNormal,
        mid.size()).ToLocal(&js_mid)) return false;
    Local<String> func_name;
    Local<Value> func_val;
    Local<Function> func;
    Local<Value> args_f[] = {name,js_mid,err_obj};
    if(!(cc->try_get_str(client_connection::FAIL_MODULE).ToLocal(&func_name) &&
        context->Global()->Get(context,func_name).ToLocal(&func_val) &&
        to_function(cc->isolate,func_val).ToLocal(&func)) ||
        func->Call(context,
            context->Global(),
            sizeof(args_f)/sizeof(Local<Value>),
            args_f).IsEmpty()) return false;

    return true;
}

void wasm_instantiate_fail(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1) {
        throw_js_typeerror(isolate,"expected one argument");
        return;
    }

    Local<Object> vals;
    Local<Value> name;
    Local<Value> e_node;
    if(!(args.Data()->ToObject(context).ToLocal(&vals) &&
        vals->Get(context,0).ToLocal(&name) &&
        vals->Get(context,1).ToLocal(&e_node))) return;

    auto node = reinterpret_cast<client_connection::cmod_list_data*>(Local<External>::Cast(e_node)->Value());

    notify_module_failure(cc,context,name,node->first->id,args[0]);
}

void wasm_compile_success(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1 || !args[0]->IsWebAssemblyCompiledModule()) {
        throw_js_typeerror(isolate,"expected web assembly module");
        return;
    }

    auto mod = Local<WasmCompiledModule>::Cast(args[0]);
    cached_module cache{mod->GetTransferrableModule()};

    Local<Object> vals;
    Local<Value> name;
    Local<Value> e_node;
    if(!(args.Data()->ToObject(context).ToLocal(&vals) &&
        vals->Get(context,0).ToLocal(&name) &&
        vals->Get(context,1).ToLocal(&e_node))) return;

    auto node = reinterpret_cast<client_connection::cmod_list_data*>(Local<External>::Cast(e_node)->Value());

    try {
        {
            uvwrap::mutex_scope_lock lock{program->module_cache_lock};
            std::swap(*node->first->get(lock),cache);
        }

        assert(cache.tag == cached_module::UNCOMPILED_WASM);
        if(cache.data.buff.waiting.size()) {
            try {
                for(auto &w : cache.data.buff.waiting) {
                    auto w_cc = program->get_connection(w.cc_id);
                    if(w_cc) w_cc->register_other_task<module_task>(w.name,w.context_id,node->first);
                }
            } catch(std::exception &e) {
                /* The waiting list was already swapped. At least one thread is
                going to wait indefinitely. */
                irrecoverable_failure(e);
            }
        }
    } catch(std::exception &e) {
        throw_js_error(isolate,e.what());
        return;
    }

    call_webassembly_func(
        cc,
        context,
        mod,
        args.Data(),
        &wasm_instantiate_success,
        &wasm_instantiate_fail,
        client_connection::INSTANTIATE);
}

void wasm_compile_fail(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1) {
        throw_js_typeerror(isolate,"expected one argument");
        return;
    }

    Local<Object> vals;
    Local<Value> name;
    Local<Value> e_node;
    if(!(args.Data()->ToObject(context).ToLocal(&vals) &&
        vals->Get(context,0).ToLocal(&name) &&
        vals->Get(context,1).ToLocal(&e_node))) return;

    auto node = reinterpret_cast<client_connection::cmod_list_data*>(Local<External>::Cast(e_node)->Value());

    String::Utf8Value msg_tmp{isolate,args[0]};
    if(!*msg_tmp) return;

    std::vector<waiting_mod_req> waiting;
    auto &mid = node->first->id;

    try {
        {
            uvwrap::mutex_scope_lock lock{program->module_cache_lock};
            auto cmod = node->first->get(lock);
            assert(cmod->tag == cached_module::UNCOMPILED_WASM);
            cmod->data.buff.started = 0;
            std::swap(cmod->data.buff.waiting,waiting);
        }
        --cc->compiling;

        if(waiting.size()) {
            try {
                std::string std_msg{*msg_tmp,static_cast<size_t>(msg_tmp.length())};

                for(auto &w : waiting) {
                    auto w_cc = program->get_connection(w.cc_id);
                    if(w_cc) w_cc->register_other_task<module_error_task>(w.name,mid,std_msg,w.context_id);
                }
            } catch(std::exception &e) {
                /* The waiting list was already swapped. At least one thread is
                going wait to indefinitely. */
                irrecoverable_failure(e);
            }
        }
    } catch(std::exception &e) { throw_js_error(isolate,e.what()); }

    notify_module_failure(cc,context,name,mid,args[0]);
}

/* This not only retrieves a cached module, it compiles it if it hasn't already
been compiled. */
void getCachedMod(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    if(args.Length() < 1) return;

    auto isolate = args.GetIsolate();
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();
    TryCatch trycatch(isolate);

    auto node = reinterpret_cast<client_connection::cmod_list_data*>(Local<External>::Cast(args.Data())->Value());
    String::Utf8Value name{isolate,args[0]};
    if(!*name) return;

    try {
        /* It is safe to access the contents without holding
        program->module_cache_lock if and only if one of the following are true:
        "tag" is JS or WASM; or "data.buff.started" was false. Otherwise the
        contents could be altered from another thread, and in the latter case,
        "data.buff.started" and "data.buff.waiting" are still not safe to
        access. */
        cached_module *cmod;
        {
            uvwrap::mutex_scope_lock lock{program->module_cache_lock};
            cmod = node->first->get(lock);

            if(cmod->is_uncompiled()) {
                if(cmod->data.buff.started) {
                    /* another thread is compiling this module, wait for it to
                    finish */
                    cmod->data.buff.waiting.emplace_back(
                        std::string{*name,size_t(name.length())},
                        cc->connection_id,
                        client_connection::get_context_id(context));

                    args.GetReturnValue().SetNull();
                    return;
                }

                cmod->data.buff.started = cc->connection_id;
                ++cc->compiling;
            }
        }

        switch(cmod->tag) {
        case cached_module::WASM:
            {
                Local<WasmCompiledModule> mod;
                auto vals = Array::New(isolate,2);
                if(!WasmCompiledModule::FromTransferrableModule(isolate,cmod->data.wasm).ToLocal(&mod) ||
                    vals->Set(context,0,args[0]).IsNothing() ||
                    vals->Set(context,1,args.Data()).IsNothing()) break;

                if(!call_webassembly_func(
                    cc,
                    context,
                    mod,
                    vals,
                    &wasm_instantiate_success,
                    &wasm_instantiate_fail,
                    client_connection::INSTANTIATE)) break;

                args.GetReturnValue().SetNull();
                return;
            }
            break;
        case cached_module::JS:
            {
                auto &cache = cmod->data.js;

                Local<String> script_str, str_string;
                if(!(String::NewFromUtf8(
                        isolate,
                        cache.code.data(),
                        NewStringType::kNormal,
                        cache.code.size()).ToLocal(&script_str) &&
                    cc->try_get_str(client_connection::STR_STRING).ToLocal(&str_string))) break;

                ScriptCompiler::Source source{
                    script_str,
                    ScriptOrigin{str_string},
                    new ScriptCompiler::CachedData(cache.data.get(),cache.length)};

                try {
                    execute_as_module exec{cc};
                    args.GetReturnValue().Set(exec(cc,check_r(ScriptCompiler::Compile(
                        exec.context,
                        &source,
                        ScriptCompiler::kConsumeCodeCache))));
                } catch(bad_request_js&) {
                    break;
                }

                return;
            }
        case cached_module::UNCOMPILED_WASM_BASE64:
            {
                auto &buff = cmod->data.buff;
                auto decoded = base64_decode(buff.data.get(),buff.length);
                if(!decoded.first) {
                    throw_js_error(isolate,"invalid base64 encoded data");
                    break;
                }
                {
                    uvwrap::mutex_scope_lock lock{program->module_cache_lock};
                    std::swap(buff.data,decoded.first);
                    buff.length = decoded.second;
                    cmod->tag = cached_module::UNCOMPILED_WASM;
                }
            }
            // fall-through
        case cached_module::UNCOMPILED_WASM:
            {
                auto &buff = cmod->data.buff;

                auto vals = Array::New(isolate,2);
                if(vals->Set(context,0,args[0]).IsNothing() ||
                    vals->Set(context,1,args.Data()).IsNothing()) break;

                if(!call_webassembly_func(
                    cc,
                    context,
                    ArrayBuffer::New(isolate,buff.data.get(),buff.length),
                    vals,
                    &wasm_compile_success,
                    &wasm_compile_fail,
                    client_connection::COMPILE)) break;

                args.GetReturnValue().SetNull();
                return;
            }
        default:
            assert(cmod->tag == cached_module::UNCOMPILED_JS);
            {
                auto &buff = cmod->data.buff;
                Local<String> script_str, str_string;
                if(!(String::NewFromUtf8(
                        isolate,
                        buff.data.get(),
                        NewStringType::kNormal,
                        buff.length).ToLocal(&script_str) &&
                    cc->try_get_str(client_connection::STR_STRING).ToLocal(&str_string))) goto compile_error;

                ScriptCompiler::Source source{
                    script_str,
                    ScriptOrigin{str_string}};
                Local<Script> script;

                try {
                    execute_as_module exec{cc};

                    if(!ScriptCompiler::Compile(
                        exec.context,
                        &source,
                        ScriptCompiler::kProduceCodeCache).ToLocal(&script)) goto compile_error;
                    auto cache = source.GetCachedData();
                    auto cmod_b = cache ?
                        cached_module{to_std_str(isolate,script_str),cache->data,cache->length} :
                        cached_module{to_std_str(isolate,script_str),nullptr,0};

                    {
                        uvwrap::mutex_scope_lock lock{program->module_cache_lock};
                        std::swap(cmod_b,*cmod);
                    }

                    try {
                        for(auto &w : cmod_b.data.buff.waiting) {
                            auto w_cc = program->get_connection(w.cc_id);
                            if(w_cc) w_cc->register_other_task<module_task>(w.name,w.context_id,node->first);
                        }
                    } catch(std::exception &e) {
                        /* The waiting list was already swapped. At least one thread
                        is going wait to indefinitely. */
                        irrecoverable_failure(e);
                    }

                    args.GetReturnValue().Set(exec(cc,script));
                } catch(bad_request_js&) {
                    break;
                }
                return;
            }
        }

        trycatch.ReThrow();
        return;

    compile_error:
        assert(cmod->is_uncompiled());

        std::vector<waiting_mod_req> waiting;

        {
            uvwrap::mutex_scope_lock lock{program->module_cache_lock};

            cmod->data.buff.started = 0;
            --cc->compiling;

            std::swap(waiting,cmod->data.buff.waiting);
        }

        try {
            if(cc->closed) {
                // let another thread compile the module
                for(auto &w : waiting) {
                    auto w_cc = program->get_connection(w.cc_id);
                    if(w_cc) w_cc->register_other_task<module_task>(w.name,w.context_id,node->first);
                }
            } else {
                std::string msg;
                {
                    TryCatch exc_tc{isolate};
                    String::Utf8Value exc_str{isolate,trycatch.Exception()};
                    if(*exc_str) {
                        msg = std::string{*exc_str,static_cast<size_t>(exc_str.length())};
                    } else {
                        exc_tc.Reset();
                        msg = "<failed to convert exception to string>";
                    }
                }
                for(auto &w : waiting) {
                    auto w_cc = program->get_connection(w.cc_id);
                    if(w_cc) w_cc->register_other_task<module_error_task>(w.name,node->first->id,msg,w.context_id);
                }
            }

            trycatch.ReThrow();
        } catch(std::exception &e) {
            /* The waiting list was already swapped. At least one thread is
            going wait to indefinitely. */
            irrecoverable_failure(e);
        }
    } catch(std::exception &e) { throw_js_error(isolate,e.what()); }
}

std::pair<unsigned long,v8::Local<v8::Context>> request_task::get_context(v8::Local<v8::Context> request_context) {
    auto v_itr = values.find(str_context);
    if(v_itr == values.end()) return client->new_user_context(true);

    auto val = to_value(client->isolate,request_context,v_itr->second);
    if(val->IsNullOrUndefined()) return client->new_user_context(true);

    int64_t id = check_r(val->ToInteger(request_context))->Value();
    if(id < 0) throw bad_context{};
    auto c_itr = client->contexts.find(static_cast<unsigned long>(id));
    if(c_itr == client->contexts.end()) throw bad_context{};
    return std::make_pair(c_itr->first,c_itr->second.Get(client->isolate));
}

std::vector<v8::Local<v8::Value>> get_array_items(v8::Local<v8::Array> x) {
    std::vector<v8::Local<v8::Value>> r;
    uint32_t length = x->Length();
    r.reserve(length);
    for(uint32_t i=0; i<length; ++i) r.push_back(check_r(x->Get(i)));
    return r;
}

void call_success(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1) return;

    Local<Integer> async_id_obj;
    if(!args.Data()->ToInteger(context).ToLocal(&async_id_obj)) return;
    auto async_id = static_cast<uint32_t>(async_id_obj->Value());

    TryCatch trycatch(isolate);

    Local<String> json_result;
    if(JSON::Stringify(context,args[0]).ToLocal(&json_result)) {
        response_builder b{isolate};
        b("type","\"asyncresult\"");
        b("value",*json_result);
        b("context",client_connection::get_context_id(context));
        b("id",async_id);
        b.send(reinterpret_cast<uv_stream_t*>(&cc->client));
    } else queue_script_exception(
        reinterpret_cast<uv_stream_t*>(&cc->client),
        trycatch,
        isolate,
        context,
        Just(async_id));
}

void call_fail(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    auto isolate = args.GetIsolate();
    Isolate::Scope isolate_scope{isolate};
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(args.Length() < 1) return;

    Local<Integer> async_id_obj;
    if(!args.Data()->ToInteger(context).ToLocal(&async_id_obj)) return;

    queue_script_exception(
        reinterpret_cast<uv_stream_t*>(&cc->client),
        args[0],
        isolate,
        context,
        Just(static_cast<uint32_t>(async_id_obj->Value())));
}

#define CACHED_STR(X) (client->get_str(client_connection::X))

void request_task::Run() {
    using namespace v8;

    if(client->closed) return;

    Locker locker{client->isolate};

    auto stream = reinterpret_cast<uv_stream_t*>(&client->client);

    Isolate::Scope isolate_scope{client->isolate};
    HandleScope handle_scope{client->isolate};

    if(client->request_context.IsEmpty()) {
        client->request_context.Reset(client->isolate,Context::New(client->isolate));
    }
    Local<Context> request_context = client->request_context.Get(client->isolate);
    Context::Scope req_context_scope{request_context};

    TryCatch trycatch(client->isolate);

    try {
        switch(type) {
        case EVAL:
            {
                Local<String> code = check_r(check_r(get_value(str_code,request_context))->ToString(request_context));
                auto context = get_context(request_context);
                Context::Scope context_scope(context.second);

                Local<Script> compiled;
                Local<Value> result;
                Local<String> json_result;
                if(Script::Compile(context.second,code).ToLocal(&compiled) &&
                        compiled->Run(context.second).ToLocal(&result) &&
                        JSON::Stringify(context.second,result).ToLocal(&json_result)) {
                    response_builder b{client->isolate};
                    b("type","\"result\"");
                    b("context",context.first);
                    b("value",*json_result).send(stream);
                } else if(!client->closed) {
                    queue_script_exception(stream,trycatch,client->isolate,context.second);
                }
            }
            break;
        case EXEC:
            {
                Local<String> code = check_r(get_value(str_code,request_context)
                    ->ToString(request_context));
                auto context = get_context(request_context);
                Context::Scope context_scope(context.second);

                Local<Script> compiled;
                if(Script::Compile(context.second,code).ToLocal(&compiled) && !compiled->Run(context.second).IsEmpty()) {
                    response_builder b{client->isolate};
                    b("type","\"success\"");
                    b("context",context.first);
                    b.send(stream);
                } else if(!client->closed) {
                    queue_script_exception(stream,trycatch,client->isolate,context.second);
                }
            }
            break;
        case CALL:
            {
                bool async = check_r(get_value(str_async,request_context)
                    ->ToBoolean(request_context))->Value();
                Local<String> fname = check_r(get_value(str_func,request_context)
                    ->ToString(request_context));
                Local<Value> tmp = get_value(str_args,request_context);

                if(tmp->IsArray()) {
                    Local<Array> args = Local<Array>::Cast(tmp);
                    auto context = get_context(request_context);
                    Context::Scope context_scope(context.second);

                    tmp = check_r(context.second->Global()->Get(context.second,fname));
                    if(tmp->IsFunction()) {
                        Local<Function> func = Local<Function>::Cast(tmp);
                        Local<Value> result;
                        auto args_v = get_array_items(args);
                        if(func->Call(context.second,context.second->Global(),static_cast<int>(args_v.size()),args_v.data()).ToLocal(&result)) {
                            if(async) {
                                Local<Promise> p;
                                if(result->IsPromise()) p = Local<Promise>::Cast(result);
                                else {
                                    tmp = check_r(
                                        check_r(
                                            check_r(
                                                context.second->Global()->Get(context.second,CACHED_STR(PROMISE))
                                            )->ToObject(context.second)
                                        )->Get(context.second,CACHED_STR(RESOLVE)));
                                    if(!tmp->IsFunction()) {
                                        queue_response(
                                            stream,
                                            copy_string(R"({"type":"error","errtype":"internal","message":"Promise.resolve is not a function"})"));
                                        break;
                                    }

                                    Local<Value> args_a[] = {result};
                                    if(!Local<Function>::Cast(tmp)->Call(
                                            context.second,
                                            context.second->Global(),
                                            sizeof(args_a)/sizeof(Local<Value>),
                                            args_a).ToLocal(&result)) {
                                        goto user_exception;
                                    }
                                    if(!result->IsPromise()) {
                                        queue_response(
                                            stream,
                                            copy_string(R"({"type":"error","errtype":"internal","message":"Promise.resolve did not return a promise"})"));
                                        break;
                                    }
                                    p = Local<Promise>::Cast(result);
                                }

                                auto async_id = client->next_async_id++;
                                auto closure_val = Integer::NewFromUnsigned(client->isolate,async_id);

                                check_r(
                                    check_r(p->Then(context.second,
                                        check_r(Function::New(context.second,&call_success,closure_val)))
                                    )->Catch(context.second,
                                        check_r(Function::New(context.second,&call_fail,closure_val))));

                                response_builder b{client->isolate};
                                b("type","\"resultpending\"");
                                b("id",async_id);
                                b("context",context.first);
                                b.send(stream);
                            } else {
                                Local<String> json_result;
                                if(JSON::Stringify(context.second,result).ToLocal(&json_result)) {
                                    response_builder b{client->isolate};
                                    b("type","\"result\"");
                                    b("value",*json_result);
                                    b("context",context.first);
                                    b.send(stream);
                                } else goto user_exception;
                            }
                        } else {
user_exception:
                            if(!client->closed) {
                                queue_script_exception(stream,trycatch,client->isolate,context.second);
                            }
                        }
                    } else {
                        string_builder b;
                        b.add_string(R"({"type":"error","errtype":"request","message":")");
                        String::Value str_val{client->isolate,fname};
                        if(!*str_val) throw bad_request_js{};
                        b.add_escaped(str_val);
                        b.add_string(R"( is not a function"})");
                        queue_response(
                            stream,
                            b.get_string());
                    }
                } else {
                    queue_response(
                        stream,
                        copy_string(R"({"type":"error","errtype":"request","message":"args must be an array"})"));
                }
            }
            break;
        case CREATE_CONTEXT:
            {
                unsigned long id = client->new_user_context(false).first;

                response_builder{client->isolate}
                    ("type","\"success\"")
                    ("context",id).send(stream);
            }
            break;
        case DESTROY_CONTEXT:
            {
                int64_t id = check_r(get_value(str_context,request_context)
                    ->ToInteger(request_context))->Value();
                if(id < 0) throw bad_context{};
                auto itr = client->contexts.find(static_cast<unsigned long>(id));
                if(itr == client->contexts.end()) throw bad_context{};
                client->contexts.erase(itr);

                response_builder{client->isolate}
                    ("type","\"success\"")
                    ("context",id).send(stream);
            }
            break;
        default:
            assert(type == GETSTATS);
            {
                string_builder b;
                b.add_string(R"({"type":"result","value":{"connections":)");
                b.add_integer(program->connection_count());
                b.add_string(R"(,"cacheitems":)");
                b.add_integer(program->cache_count());
                b.add_string("}}");
                queue_response(
                    stream,
                    b.get_string());
            }
            break;
        }
    } catch(std::bad_alloc&) {
        if(!client->closed)
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"memory","message":"insufficient memory"})"));
    } catch(bad_request_js&) {
        if(!client->closed)
            queue_js_exception(stream,trycatch,client->isolate,request_context);
    } catch(bad_context&) {
        if(!client->closed)
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"context","message":"no such context"})"));
    } catch(...) {
        if(!client->closed)
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"internal","message":"unexpected error type"})"));
    }
}

void module_task::Run() {
    using namespace v8;

    if(client->closed) return;

    Locker locker{client->isolate};
    Isolate::Scope isolate_scope{client->isolate};
    HandleScope handle_scope{client->isolate};

    try {
        auto itr = client->contexts.find(context_id);
        if(itr == client->contexts.end()) {
            /* the context has been closed, so we are done here */
            return;
        }
        auto context = itr->second.Get(client->isolate);
        Context::Scope context_scope(context);

        Local<String> js_name;
        if(!String::NewFromUtf8(
            client->isolate,
            name.data(),
            v8::NewStringType::kNormal,
            name.size()).ToLocal(&js_name))
        {
            fprintf(stderr,"failed to load module name\n");
            client->close_async();
            return;
        }

        TryCatch trycatch{client->isolate};
        Local<Value> js_mid = Null(client->isolate);
        try {
            js_mid = from_std_str(client->isolate,cmod->id);
            Local<Function> resolve_f = check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(RESOLVE_MODULE)))));

            auto node = client->new_cmod_node(cmod);
            Local<Function> getcached_f = check_r(Function::New(context,&getCachedMod,node));

            Local<Value> args[] = {js_name,js_mid,getcached_f};
            check_r(resolve_f->Call(
                context,
                context->Global(),
                sizeof(args)/sizeof(Local<Value>),
                args));
        } catch(bad_request_js&) {
            if(client->closed || !trycatch.CanContinue()) return;
            if(trycatch.HasTerminated()) client->isolate->CancelTerminateExecution();
            try {
                Local<Value> args[] = {
                    js_name,
                    js_mid,
                    check_r(trycatch.Exception()->ToString(context))
                };
                check_r(check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(FAIL_MODULE)))))->Call(
                    context,
                    context->Global(),
                    sizeof(args)/sizeof(Local<Value>),
                    args));
            } catch(bad_request_js&) {
                if(client->closed) return;
                fprintf(stderr,"error occurred while trying to report another error\n");
                client->close_async();
            }
        }
    } catch(std::bad_alloc&) {
        fprintf(stderr,"not enough memory to handle request\n");

        /* there's probably not enough memory for this either, but it doesn't
        hurt to try (if it fails, the process will be shut down, which is what
        we want) */
        client->close_async();
    } catch(...) {
        fprintf(stderr,"unexpected error type\n");
        client->close_async();
    }
}

void module_error_task::Run() {
    using namespace v8;

    if(client->closed) return;

    Locker locker{client->isolate};
    Isolate::Scope isolate_scope{client->isolate};
    HandleScope handle_scope{client->isolate};

    try {
        auto itr = client->contexts.find(context_id);
        if(itr == client->contexts.end()) {
            /* the context has been closed, so we are done here */
            return;
        }
        auto context = itr->second.Get(client->isolate);
        Context::Scope context_scope(context);

        auto js_mid = from_std_str(client->isolate,mid);
        Local<Value> args[] = {
            from_std_str(client->isolate,name),
            js_mid,
            from_std_str(client->isolate,msg)
        };
        check_r(check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(FAIL_MODULE)))))->Call(
            context,
            context->Global(),
            sizeof(args)/sizeof(Local<Value>),
            args));
    } catch(bad_request_js&) {
        if(!client->closed)
            fprintf(stderr,"error occurred while trying to report another error\n");
        client->close_async();
    } catch(...) {
        fprintf(stderr,"unexpected error type\n");
        client->close_async();
    }
}

#undef CACHED_STR

void program_t::audit_module_cache() noexcept {
    try {
        uvwrap::mutex_scope_lock lock{func_dispatch_lock};
        for(auto &ptr : module_cache) {
            auto item = ptr->get(lock);
            if(item->is_uncompiled() && item->data.buff.started) {
                auto cc = get_connection(item->data.buff.started);
                if(!cc) {
                    item->data.buff.started = 0;
                    for(auto &w : item->data.buff.waiting) {
                        auto w_cc = program->get_connection(w.cc_id);
                        if(w_cc) w_cc->register_other_task<module_task>(w.name,w.context_id,ptr);
                    }
                    item->data.buff.waiting.clear();
                }
            }
        }
    } catch(std::exception &e) {
        irrecoverable_failure(e);
    }
}

struct bad_request_error {
    const char *const msg;
    bad_request_error(const char *msg) : msg{msg} {}
};

almost_json_parser::parsed_value &get_parsed_value(command_dispatcher *cd,const std::string &key) {
    auto val = cd->command_buffer.value.find(key);
    if(val == cd->command_buffer.value.end()) {
        throw bad_request_error{"missing value"};
    }
    return val->second;
}

std::string &get_str_value(command_dispatcher *cd,const std::string &key) {
    auto &val = get_parsed_value(cd,key);
    if(!val.is_string) throw bad_request_error{"invalid value type"};
    return val.data;
}

unsigned long long get_ulonglong_value(command_dispatcher *cd,const std::string &key) {
    /* it doesn't matter whether the value is a string or not, JS is weakly
    typed */
    auto &val = get_parsed_value(cd,key);
    try {
        return std::stoull(val.data);
    } catch(...) {
        throw bad_request_error{"invalid value type"};
    }
}

void program_t::register_task() {
    try {
        int type_i = type_str_to_index(
            reinterpret_cast<uv_stream_t*>(&stdout.data),
            MODULE_REQ_COUNT_TYPES,
            module_request_types);
        if(type_i == -1) return;

        auto type = static_cast<module_req_type_t>(type_i);
        if(type == PURGE_CACHE) {
            auto itr = command_buffer.value.find(str_ids);
            if(itr != command_buffer.value.end()) {
                if(itr->second.is_string) throw bad_request_error{"invalid value type"};

                uvwrap::mutex_scope_lock lock{module_cache_lock};

                if(itr->second.data == "null") {
                    module_cache.clear();
                } else {
                    almost_json_parser::array_parser p;
                    try {
                        p.feed(itr->second.data.data(),itr->second.data.size());
                        p.finish();
                    } catch(almost_json_parser::syntax_error&) {
                        throw bad_request_error{"invalid JSON"};
                    }

                    for(auto &item : p.value) {
                        if(!item.is_string) throw bad_request_error{"invalid value type"};
                        auto itr = module_cache.find(item.data);
                        if(itr != module_cache.end()) module_cache.erase(itr);
                    }
                }

                return;
            }
        }

        connection_id_t cc_id;
        unsigned long context_id;

        try {
            cc_id = get_ulonglong_value(this,str_connection);
            context_id = static_cast<unsigned long>(get_ulonglong_value(this,str_context));
        } catch(...) {
            throw bad_request_error{"invalid connection or context id"};
        }

        std::unique_ptr<v8::Task> task;

        auto cc = get_connection(cc_id);

        switch(type) {
        case MODULE:
            /* Stash the module data. It will be compiled by one of the
            connections's threads */
            {
                auto &name = get_str_value(this,str_name);
                auto &mid = get_str_value(this,str_id);
                auto &mtype = get_str_value(this,str_modtype);
                auto &data = get_str_value(this,str_value);

                cached_module::tag_t tag;
                if(mtype == "js") tag = cached_module::UNCOMPILED_JS;
                else if(mtype == "wasm") tag = cached_module::UNCOMPILED_WASM_BASE64;
                else throw bad_request_error{"invalid module type"};
                if(data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    throw bad_request_error{"payload too large"};
                }

                auto cmod = std::make_shared<module_cache_item>(
                    mid,
                    data.data(),
                    static_cast<int>(data.size()),
                    tag);

                {
                    uvwrap::mutex_scope_lock lock{module_cache_lock};
                    module_cache.insert(cmod);
                }

                if(cc) task.reset(new module_task(cc,name,context_id,cmod));
            }
            break;
        case MODULE_CACHED:
            if(cc) {
                auto &mid = get_str_value(this,str_id);

                std::shared_ptr<module_cache_item> cmod;
                {
                    uvwrap::mutex_scope_lock lock{module_cache_lock};
                    auto itr = program->module_cache.find(mid);
                    if(itr != program->module_cache.end()) cmod = *itr;
                }
                if(!cmod) throw bad_request_error{"no such module"};

                task.reset(new module_task(
                    cc,
                    get_str_value(this,str_name),
                    context_id,
                    cmod));
            }
            break;
        default:
            assert(type == MODULE_ERROR);
            if(cc) task.reset(new module_error_task(
                cc,
                get_str_value(this,str_name),
                "",
                get_str_value(this,str_msg),
                context_id));
            break;
        }

        if(cc) cc->tloop->PostTask(std::move(task));
    } catch(bad_request_error &e) {
        string_builder b;
        b.add_string(R"({"type":"error","errtype":"request","message":")");
        b.add_escaped_ascii(e.msg);
        b.add_string("\"}");
        queue_response(
            stdout,
            b.get_string());
    }
}

void on_new_connection(uv_stream_t *server_,int status) noexcept {
    if(status < 0) {
        fprintf(stderr,"connection error: %s\n",uv_strerror(status));
        return;
    }

    try {
        auto cc = new client_connection;

        ASSERT_UV_RESULT(uv_accept(server_,cc->client.as_uv_stream()));

        cc->client.read_start(
            [](uv_stream_t *s,ssize_t read,const uv_buf_t *buf) {
                auto cc = reinterpret_cast<client_connection*>(s->data);
                cc->handle_input(s,read,buf);
            });
    } catch(std::exception &e) {
        fprintf(stderr,"error: %s\n",e.what());
        return;
    }
}

void signal_handler(uv_signal_t*,int) {
    program->sig_request.stop(std::nothrow);
    program->shutting_down = true;
    fprintf(stderr,"shutting down\n");
    client_connection::close_all();
    program->main_loop.stop();
}

v8::StartupData create_snapshot() {
    using namespace v8;

    v8::StartupData r{nullptr,0};

    SnapshotCreator creator{external_references};
    auto isolate = creator.GetIsolate();
    {
        Isolate::Scope isolate_scope{isolate};
        HandleScope handle_scope{isolate};

        auto context = Context::New(isolate,nullptr,get_globals(isolate));
        Context::Scope context_scope{context};

        Local<String> code;
        Local<Script> compiled;
        if(
            !String::NewFromUtf8(isolate,startup_code,NewStringType::kNormal).ToLocal(&code) ||
            !Script::Compile(context,code).ToLocal(&compiled) ||
            compiled->Run(context).IsEmpty()) return r;

        creator.SetDefaultContext(context);
    }
    r = creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);

    return r;
}

program_t::program_t() :
    server{main_loop},
    sig_request{main_loop},
    stdin{main_loop},
    stdout{main_loop},
    snapshot{create_snapshot()},
    next_conn_index{0},
    shutting_down{false},
    console_enabled{false},
    func_dispatcher{
        main_loop,
        [](uv_async_t *handle) {
            reinterpret_cast<program_t*>(handle->data)->run_functions();
        },
        this
    }
{
    if(!snapshot.data) {
        throw std::runtime_error{"failed to create start-up snapshot"};
    }

    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    create_params.snapshot_blob = &snapshot;
    create_params.external_references = external_references;

    stdin.data.data = this;
}

program_t::~program_t() {
    delete[] snapshot.data;
    delete create_params.array_buffer_allocator;
}

int main(int argc, char* argv[]) {
#ifndef NDEBUG
    _master_thread = uv_thread_self();
#endif

    if(argc != 2) {
        fprintf(stderr,"exactly one argument is required\n");
        return 1;
    }

    if(uv_guess_handle(0) == UV_FILE) {
        // libuv will abort if uv_read_start is used on a file
        fprintf(stderr,"standard-in cannot be a file\n");
        return 1;
    }

    try {
        v8::V8::InitializeICUDefaultLocation(argv[0]);
        v8::V8::InitializeExternalStartupData(argv[0]);

        program_t p;
        program = &p;

        // TODO: allow enabling the console with a flag or environment variable
#ifndef NDEBUG
        p.console_enabled = true;
#endif

        p.sig_request.start(&signal_handler,SIGINT);
        p.server.bind(argv[1]);
        p.server.listen(128,&on_new_connection);
        p.stdin.open(0);
        p.stdin.read_start([](uv_stream_t *s,ssize_t read,const uv_buf_t *buf) noexcept {
            auto p = reinterpret_cast<program_t*>(s->data);
            p->handle_input(reinterpret_cast<uv_stream_t*>(&p->stdout.data),read,buf);
        });
        p.stdout.open(1);

        /* This is to let a parent process know that a socket/pipe is ready.
        fwrite is used because we need to include the nul character in the
        output. */
        const char msg[] = "{\"type\":\"ready\"}";
        fwrite(msg,1,sizeof(msg),stdout);
        fflush(stdout);

        p.main_loop.run(UV_RUN_DEFAULT);

#ifndef NDEBUG
        size_t runners = p.platform.remaining_runners();
        fprintf(stderr,"%zu remaining runner%s\n",runners,runners==1 ? "" : "s");
#endif
    } catch(std::exception &e) {
        fprintf(stderr,"%s\n",e.what());
        return 2;
    }

    return 0;
}

