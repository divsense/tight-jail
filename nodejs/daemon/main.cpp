#include <libplatform/libplatform.h>
#include <v8.h>
#include <memory>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>
#include <functional>
#include <atomic>
#include <type_traits>
#include <exception>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "almost_json_parser.hpp"
#include "uvwrap.hpp"


#if V8_MAJOR_VERSION >= 7
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length(isolate))
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(isolate,data))
#else
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length())
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(data))
#endif

#define RESOLVE_MODULE_FUNC_NAME "__resolveModule"
#define RESOLVE_READY_MODULE_FUNC_NAME "__resolveReadyModule"
#define FAIL_MODULE_FUNC_NAME "__failModule"

/* There are two levels of module cache. Each context has a module instance
cache, but there is also a global cache that contains modules that have been
compiled but not instantiated. */
const char startup_code[] = R"(
class JailImportError extends Error {}
JailImportError.prototype.name = 'JailImportError';

class __PendingMod {
  constructor(resolve1,reject1) {
    this.callbacks = [[resolve1,reject1]];
    this.aliases = [];
  }

  add(resolve,reject) {
    this.callbacks.push([resolve,reject]);
  }

  mergeFrom(b) {
    b.callbacks.forEach(item => this.callbacks.push(item));
    b.aliases.forEach(item => this.aliases.push(item));
  }

  resolve(x) {
    for(let c of this.callbacks) c[0](x);
  }

  reject(x) {
    for(let c of this.callbacks) c[1](x);
  }
}

var __moduleCacheByName = {};
var __moduleCacheById = {};

function jimport(m) {
  if(r instanceof __PendingMod) return r.promise;
  if(r !== undefined) return Promise.resolve(r);

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

  return r;
}

function __resolveModule(name,mid,getCachedGlobal) {
  let pending = __moduleCacheByName[name];
  let r = __moduleCacheById[normName];
  if(r isntanceof __PendingMod) {
    if(pending !== r) {
      r.mergeFrom(pending);
      __moduleCacheByName[name] = r;
    }
  } else if(r === undefined) {
    try {
      r = getCachedGlobal();
    } catch(e) {
      delete __PendingMod[name];
      pending.reject(e);
    }

    if(r === null) {
      /* The module is being compiled. This function or __resolveReadyModule
      will be called when the module is ready. */
      __moduleCacheById[mid] = pending;
    } else {
      __moduleCacheByName[name] = r;
      __moduleCacheById[mid] = r;
      pending.resolve(r);
    }
  } else {
    __moduleCacheByName[name] = r;
    pending.resolve(r);
  }
}

function __resolveReadyModule(name,mid,mod) {
  let pending = __moduleCacheByName[name];
  __moduleCacheByName[name] = mod;
  __moduleCacheById[mid] = mod;
  pending.resolve(mod);
}

function __failModule(name,message) {
  let pending = __moduleCacheByName[name];
  delete __moduleCacheByName[name];
  pending.reject(new JailImportError(message));
}
)";


struct program_t;
program_t *program;
#ifndef NDEBUG
uv_thread_t _master_thread;
#endif

#define ENSURE_MAIN_THREAD assert(uv_thread_equal(&_master_thread,&uv_thread_self()))


template<size_t N> char *copy_string(const char (&str)[N]) {
    char *r = new char[N];
    memcpy(r,str,N);
    return r;
}

void queue_response(uv_stream_t *stream,char *msg,ssize_t len=-1);

struct js_cache {
    std::string code;
    std::unique_ptr<uint8_t[]> data;
    int length;
};
struct waiting_mod_req {
    std::string name;
    unsigned long long cc_id;
    unsigned long context_id;
};
struct uncompiled_buffer {
    std::unique_ptr<char[]> data;
    int length;

    /* if "started" is true, then a thread is already compiling the module and
    other threads will add an entry to "waiting" if they need the same module.
    */
    bool started;
    std::vector<waiting_mod_req> waiting;
};
struct cached_module {
    enum tag_t {UNCOMPILED_WASM,UNCOMPILED_JS,WASM,JS} tag;
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

    cached_module(std::string code,const uint8_t *compile_data,int length) : tag{JS} {
        new(&data.js) js_cache({code,std::unique_ptr<uint8_t[]>{new uint8_t[length]},length});
        memcpy(data.js.data.get(),compile_data,length);
    }

    cached_module(const char *mod_data,int length,tag_t t) : tag{t} {
        assert(t == UNCOMPILED_WASM || t == UNCOMPILED_JS);
        new(&data.buff) uncompiled_buffer({std::unique_ptr<char[]>{new char[length]},length,false,{}});
        memcpy(data.buff.data.get(),mod_data,length);
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
            assert(tag == UNCOMPILED_WASM || tag == UNCOMPILED_JS);
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
            assert(tag == UNCOMPILED_WASM || tag == UNCOMPILED_JS);
            new(&data.buff) uncompiled_buffer(std::move(b.data.buff));
            break;
        }
    }
};

/* a seperate struct so that "Dispose()" and "ShutdownPlatform()" get called
even if the constructor for program_t throws */
struct v8_program_t {
    v8::Platform *platform;

    v8_program_t(const char *progname) {
        v8::V8::InitializeICUDefaultLocation(progname);
        v8::V8::InitializeExternalStartupData(progname);
        platform = v8::platform::CreateDefaultPlatform();
        v8::V8::InitializePlatform(platform);
        v8::V8::Initialize();
    }

    ~v8_program_t() {
        v8::V8::Dispose();
        v8::V8::ShutdownPlatform();
        delete platform;
    }
};

struct command_dispatcher {
    almost_json_parser::parser command_buffer;

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

protected:
    ~command_dispatcher() = default;
};

void command_dispatcher::handle_input(uv_stream_t *output_s,ssize_t read,const uv_buf_t *buf) noexcept {
    auto cd = reinterpret_cast<command_dispatcher*>(output_s->data);

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
                            cd->command_buffer.feed(start,zero-start);
                            cd->command_buffer.finish();
                            cd->register_task();
                        }
                        cd->command_buffer.reset();
                        parse_error = false;

                        start = zero + 1;
                        zero = std::find(start,end,0);
                    }

                    if(!parse_error) cd->command_buffer.feed(start,end-start);
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

template<typename T> class locked_ptr {
    uvwrap::mutex_scope_lock _lock;
    T *_ptr;

public:
    locked_ptr(uvwrap::mutex_t &mut,T *_ptr) : _lock{mut}, _ptr{_ptr} {}
    T *get() const { return _ptr; }
    T *operator->() const { return _ptr; }
    T &operator*() const { return *_ptr; }
};

struct client_connection;

struct program_t : v8_program_t, command_dispatcher {
    friend struct client_connection;

    typedef std::map<std::string,cached_module> module_cache_map;
    typedef std::map<unsigned long long,client_connection*> connection_map;

    uvwrap::mutex_t module_cache_lock;
    uvwrap::loop_t main_loop;
    uvwrap::pipe_t server;
    uvwrap::signal_t sig_request;
    uvwrap::pipe_t stdin;
    uvwrap::pipe_t stdout;
    v8::StartupData snapshot;
    v8::Isolate::CreateParams create_params;
    module_cache_map module_cache;
    unsigned long long next_conn_index;

    program_t(const char *progname);
    ~program_t();

    void on_read_error() override {
        fprintf(stderr,"error in standard-in\n");
    }

    void register_task() override;

private:
    uvwrap::mutex_t func_dispatch_lock;
    uvwrap::async_t func_dispatcher;
    std::vector<std::function<void()>> func_requests;

    uvwrap::mutex_t connections_lock;
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

    client_connection *get_connection(unsigned long long id) {
        uvwrap::mutex_scope_lock lock{connections_lock};
        auto itr = connections.find(id);
        return itr == connections.end() ? nullptr : itr->second;
    }
};

struct bad_request {};
struct bad_context {};

template<typename T> v8::Local<T> check_r(v8::Local<T> x) {
    if(x.IsEmpty()) throw bad_request{};
    return x;
}

template<typename T> v8::Local<T> check_r(v8::MaybeLocal<T> x) {
    v8::Local<T> r;
    if(!x.ToLocal(&r)) throw bad_request{};
    return r;
}

std::string to_std_str(v8::Isolate *isolate,v8::Local<v8::Value> x) {
    v8::String::Utf8Value id{isolate,x};
    if(!*id) throw bad_request{};
    return std::string{*id,size_t(id.length())};
}

v8::Local<v8::String> from_std_str(v8::Isolate *isolate,const std::string &x) {
    return check_r(v8::String::NewFromUtf8(
        isolate,
        x.data(),
        v8::NewStringType::kNormal,
        x.size()));
}


struct list_node {
    list_node *prev, *next;

    list_node() : prev(nullptr), next(nullptr) {}

    void remove() {
        if(prev) prev->next = next;
        if(next) next->prev = prev;
        next = prev = nullptr;
    }

    void insert_before(list_node *node) {
        assert(!(node->next || node->prev));
        if(prev) {
            prev->next = node;
            node->prev = prev;
        }
        node->next = this;
        prev = node;
    }

    void insert_after(list_node *node) {
        assert(!(node->next || node->prev));
        if(next) {
            next->prev = node;
            node->next = next;
        }
        node->prev = this;
        next = node;
    }

protected:
    ~list_node() {
        assert(!(next || prev));
    }
};

void sendImportMessage(const v8::FunctionCallbackInfo<v8::Value> &args);
const intptr_t external_references[] = {
    reinterpret_cast<intptr_t>(&sendImportMessage),
    0};


struct client_task;

struct task_loop {
    v8::Isolate *isolate;
    std::shared_ptr<v8::TaskRunner> task_runner;
    uvwrap::loop_t loop;
    uvwrap::async_t dispatcher;
    uv_thread_t thread;
    volatile bool closing = false;

    task_loop(v8::Isolate *isolate) :
        isolate{isolate},
        task_runner{program->platform->GetForegroundTaskRunner(isolate)},
        dispatcher{
            loop,
            [](uv_async_t *handle) {
                auto tloop = reinterpret_cast<task_loop*>(handle->data);
                if(tloop->closing) {
                    tloop->loop.stop();
                } else {
                    while(v8::platform::PumpMessageLoop(program->platform,tloop->isolate))
                        tloop->isolate->RunMicrotasks();
                }
            },
            this}
    {
        int r;

        if((r = uv_thread_create(
            &thread,
            [](void *arg) {
                uv_run(reinterpret_cast<uv_loop_t*>(arg),UV_RUN_DEFAULT);
            },
            &loop.data)))
        {
            throw uvwrap::uv_error{r};
        }
    }

    ~task_loop() {
        closing = true;
        ASSERT_UV_RESULT(uv_async_send(&dispatcher.data));
        ASSERT_UV_RESULT(uv_thread_join(&thread));
        closing = false; // don't try to stop the loop again
    }

    void add_task(std::unique_ptr<v8::Task> &&task) {
        task_runner->PostTask(std::move(task));
        ASSERT_UV_RESULT(uv_async_send(&dispatcher.data));
    }
};

/* a seperate struct so that "isolate->Dispose()" gets called even if the
constructor for client_connection throws */
struct autoclose_isolate {
    v8::Isolate* isolate;

    autoclose_isolate(v8::Isolate* isolate) : isolate{isolate} {}
    ~autoclose_isolate() { isolate->Dispose(); }
};

struct client_connection : public autoclose_isolate, command_dispatcher {
    typedef std::map<unsigned long,v8::UniquePersistent<v8::Context>> context_map;

    static constexpr int CONTEXT_ID_INDEX = 0;

    // used for "getstats"
    static std::atomic<unsigned long> conn_count;

    uv_pipe_t client;

    task_loop tloop;
    v8::UniquePersistent<v8::Context> request_context;
    client_task *requests_head;
    context_map contexts;
    unsigned long next_id;
    unsigned long long connection_id;

    v8::UniquePersistent<v8::ObjectTemplate> globals_template;

    bool closed;

    enum {
        STR_STRING=0,
        RESOLVE_MODULE,
        RESOLVE_READY_MODULE,
        FAIL_MODULE,
        EXPORTS,
        COUNT_STR
    };

    v8::UniquePersistent<v8::String> str[COUNT_STR];
    static const char *str_raw[COUNT_STR];

    client_connection() :
            autoclose_isolate{v8::Isolate::New(program->create_params)},
            tloop{isolate},
            requests_head{nullptr},
            next_id{1},
            closed{false}
    {
        client.data = this;

        isolate->SetData(0,this);

        uvwrap::mutex_scope_lock lock{program->connections_lock};
        connection_id = program->next_conn_index++;
        program->connections.emplace(connection_id,this);
        ++conn_count;
    }

    ~client_connection() {
        assert(!requests_head && closed);
        --conn_count;

        /* all the JS references need to be freed before calling
           isolate->Dispose */
        for(auto &item : str) item.Reset();
        request_context.Reset();
        globals_template.Reset();
        contexts.clear();
    }

    v8::Local<v8::ObjectTemplate> get_globals();
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
        tloop.add_task(std::make_shared<TaskT>(std::forward<T>(x)...));
    }

    static unsigned long get_context_id(v8::Local<v8::Context> c) {
        return reinterpret_cast<uintptr_t>(c->GetAlignedPointerFromEmbedderData(CONTEXT_ID_INDEX));
    }

    int type_str_to_index(int size,std::string *types) {
        auto type = command_buffer.value.find(request_task::str_type);
        if(type == command_buffer.value.end() || !type->second.is_string) {
            queue_response(
                reinterpret_cast<uv_stream_t*>(&client),
                copy_string(R"({"type":"error","errtype":"request","message":"invalid request"})"));
            return -1;
        }

        for(int i=0; i<size; ++i) {
            if(types[i] == type->second.data) {
                return i;
            }
        }

        queue_response(
            reinterpret_cast<uv_stream_t*>(&client),
            copy_string(R"({"type":"error","errtype":"request","message":"unknown request type"})"));
        return -1;
    }
};

std::atomic<unsigned long> client_connection::conn_count{0};

v8::Local<v8::ObjectTemplate> get_globals(v8::Isolate *isolate) {
    auto ft = v8::ObjectTemplate::New(isolate);

    ft->Set(
        check_r(v8::String::NewFromUtf8(
            isolate,
            "__sendImportMessage",
            v8::NewStringType::kNormal)),
        v8::FunctionTemplate::New(isolate,&sendImportMessage));

    return ft;
}

v8::Local<v8::ObjectTemplate> client_connection::get_globals() {
    if(globals_template.IsEmpty()) {
        auto ft = ::get_globals(isolate);
        globals_template.Reset(isolate,ft);

        return ft;
    }
    return globals_template.Get(isolate);
}

std::pair<unsigned long,v8::Local<v8::Context>> client_connection::new_user_context(bool weak) {
    typedef std::pair<client_connection*,context_map::iterator> map_ref;

    unsigned long id = next_id++;
    auto c = v8::Context::New(isolate,nullptr,get_globals());
    c->SetAlignedPointerInEmbedderData(CONTEXT_ID_INDEX,reinterpret_cast<void*>(uintptr_t(id)));
    auto itr = contexts.emplace(id,v8::UniquePersistent<v8::Context>{isolate,c}).first;
    if(weak) itr->MakeWeak(
        new map_ref(this,itr),
        [](Persistant<Value> obj,void *param) {
            auto ref = reinterpret_cast<map_ref*>(param);
            ref->first->contexts.erase(ref->second);
            obj.Dispose();
            delete ref;
        }
    );
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
    }
    return str[i].Get(isolate);
}

const char *client_connection::str_raw[client_connection::COUNT_STR] = {
    "string",
    RESOLVE_MODULE_FUNC_NAME,
    RESOLVE_READY_MODULE_FUNC_NAME
    FAIL_MODULE_FUNC_NAME,
    "exports"
};

void client_connection::close_async() noexcept {
    try {
        program->run_on_master_thread([] { this->close(); });
    } catch(...) {
        printf(stderr,"unable to schedule connection to close\n");
        std::terminate();
    }
}

struct write_request {
    uv_write_t req;
    std::unique_ptr<char[]> msg;

    write_request(char *msg) : msg(msg) {
        req.data = this;
    }
};

struct client_task : private list_node, public v8::Task {
    friend struct client_connection;

    client_connection *client;
    bool cancelled;

    virtual ~client_task() {
        if(client->requests_head == this) client->requests_head = static_cast<client_task*>(next);
        remove();
    }

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
        MODULE,
        MODULE_CACHED,
        GETSTATS,
        COUNT_TYPES
    } type;

    static const std::string request_types[COUNT_TYPES];
    static const std::string str_type;
    static const std::string str_code;
    static const std::string str_func;
    static const std::string str_args;
    static const std::string str_context;
    static const std::string str_name;
    static const std::string str_id;

    almost_json_parser::value_map values;

    void Run() override;

    v8::Local<v8::Value> get_value(const std::string &name,v8::Local<v8::Context> context) const;

private:
    request_task(client_connection *client,type_t type) :
        client_task{client},
        type{type} {}
};

struct module_task : client_task {
    std::string name;
    std::string mid;
    unsigned long context_id;
    void Run() override;
};

struct module_error_task : client_task {
    std::string name;
    std::string mid;
    std::string msg;
    unsigned long context_id;
    void Run() override;
};

const std::string request_task::request_types[request_task::COUNT_TYPES] = {
    "createcontext",
    "destroycontext",
    "eval",
    "exec",
    "call",
    "module",
    "modulecached",
    "getstats"
};

const std::string request_task::str_type = "type";
const std::string request_task::str_code = "code";
const std::string request_task::str_func = "func";
const std::string request_task::str_args = "args";
const std::string request_task::str_context = "context";
const std::string request_task::str_name = "name";
const std::string request_task::str_id = "id";

v8::Local<v8::Value> request_task::get_value(const std::string &name,v8::Local<v8::Context> context) const {
    using namespace v8;

    auto itr = values.find(name);
    if(itr == values.end()) {
        client->isolate->ThrowException(
            Exception::TypeError(
                String::NewFromUtf8(
                    client->isolate,
                    "invalid request",
                    NewStringType::kNormal).ToLocalChecked()));
        throw bad_request{};
    }

    auto r = check_r(String::NewFromUtf8(
        client->isolate,
        itr->second.data.data(),
        NewStringType::kNormal,
        itr->second.data.size()));
    return itr->second.is_string ? static_cast<Local<Value>>(r) : check_r(JSON::Parse(context,r));
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
        reinterpret_cast<uv_stream_t*>(&stream.data),
        msg,
        len);
}

std::string &get_str_value(command_dispatcher *cd,const std::string &key) {
    auto val = cd->command_buffer.value.find(key);
    if(name == cd->command_buffer.value.end() || !val->second.is_string) throw bad_request{};
    return val->second.data;
}

void client_connection::register_task() {
    int rtype = type_str_to_index(request_task::COUNT_TYPES,request_task::request_types);
    if(rtype == -1) return;

    auto task = std::make_shared<request_task>(this,static_cast<request_task::type_t>(rtype));
    std::swap(task->values,command_buffer.value);
    if(requests_head) requests_head->insert_before(task.get());
    requests_head = task.get();
    tloop.add_task(task);
}

void client_connection::close() {
    ENSURE_MAIN_THREAD;

    if(!closed) {
        closed = true;
        {
            uvwrap::mutex_scope_lock lock{program->connections_lock};
            program->connections.erase(connection_id);
        }
        isolate->TerminateExecution();

        uv_close(
            reinterpret_cast<uv_handle_t*>(&client),
            [](uv_handle_t *h) {
                auto self = reinterpret_cast<client_connection*>(h->data);
                delete self;
            });
    }
}

char hex_digit(int x) {
    assert(x >= 0 && x < 16);
    return "01234567890abcdef"[x];
}

class string_builder {
    std::vector<char> buffer;

    void add_escaped_char(char16_t c);

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

    void add_integer(long i);

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
    for(char16_t c : str) add_escaped_char(reinterpret_cast<uint16_t>(c));
}

void string_builder::add_escaped_char(uint16_t c) {
    if(c <= 0xff) {
        if(c <= 0x7f && isprint(c)) add_char(static_cast<char>(c));
        else {
            add_char('\\');
            add_char('x');
            add_char(hex_digit(c >> 4));
            add_char(hex_digit(c & 0xf));
        }
    } else {
        add_char('\\');
        add_char('u');
        add_char(hex_digit(c >> 12));
        add_char(hex_digit((c >> 8) & 0xf));
        add_char(hex_digit((c >> 4) & 0xf));
        add_char(hex_digit(c & 0xf));
    }
}

void string_builder::add_integer(long i) {
    if(i == 0) {
        add_char('0');
        return;
    }
    auto start = buffer.end();
    bool neg = false;
    unsigned long j;
    if(i < 0) {
        neg = true;
        j = static_cast<unsigned long>(-(i + 1)) + 1; // negate without overflow
    } else j = i;

    while(j) {
        add_char(hex_digit(j % 10));
        j = j / 10;
    }

    if(neg) add_char('-');

    std::reverse(start,buffer.end());
}

struct escaped {
    v8::String::Value &val;
    escaped(v8::String::Value &val) : val(val) {}
};

struct response_builder {
    string_builder build;
    v8::Isolate* isolate;

    response_builder(v8::Isolate* isolate) : isolate(isolate) {}

    void send(uv_stream_t *stream) {
        assert(build.size());
        build.add_char('}');
        queue_response(stream,build.get_string());
    }

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
        build.add_integer(static_cast<long>(value));
    }

    void add_part(v8::String *value) {
        build.add_string(isolate,value);
    }

    template<size_t N,typename T> response_builder &operator()(const char (&name)[N],const T &value) {
        add_name(name,N-1);
        build.add_char(':');
        add_part(value);

        return *this;
    }

    template<size_t N> response_builder &operator()(const char (&name)[N],escaped value) {
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

void queue_script_exception(uv_stream_t *stream,v8::TryCatch &tc,v8::Isolate *isolate,v8::Local<v8::Context> context) {
    using namespace v8;

    string_builder tmp;
    tmp.add_string(R"({"type":"resultexception","message":)");

    String::Value exc_str{isolate,tc.Exception()};

    if(*exc_str) {
        tmp.add_char('"');
        tmp.add_escaped(exc_str);
        tmp.add_char('"');

        Local<Message> exc_msg = tc.Message();
        if(!exc_msg.IsEmpty()) {
            tmp.add_string(",\"filename\":");
            String::Value filename{isolate,exc_msg->GetScriptResourceName()};
            if(*filename) {
                tmp.add_char('"');
                tmp.add_escaped(filename);
                tmp.add_char('"');
            }

            tmp.add_string(",\"lineno\":");
            Maybe<int> lineno = exc_msg->GetLineNumber(context);
            if(lineno.IsJust()) tmp.add_integer(lineno.FromJust());
        } else {
            tmp.add_string(R"(",filename":null,"lineno":null)");
        }
    } else {
        tmp.add_string(R"(null,"filename":null,"lineno":null)");
    }
    tmp.add_char('}');

    tc.Reset();

    queue_response(stream,tmp.get_string());
}

void sendImportMessage(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    if(args.Length() < 1) return;

    auto isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);

    auto stream = reinterpret_cast<uv_stream_t*>(
        &reinterpret_cast<client_connection*>(isolate->GetData(0))->client);

    v8::String::Value mname{isolate,args[0]};
    if(!*mname) return;

    v8::String::Utf8Value id{isolate,x};
    if(!*id) return;
    try {
        std::u16string req{reinterpret_cast<char16_t*>(*id),size_t(id.length())};

        program->run_on_master_thread([=] {
            string_builder tmp;
            tmp.add_string(R"({"type":"import","name":")");
            tmp.add_escaped(req);
            tmp.add_string("\"}");

            queue_response(program->stdout,tmp.get_string());
        });
    } catch(std::exception &e) {
        isolate->ThrowException(
            Exception::Error(
                String::NewFromUtf8(
                    isolate,
                    e.what(),
                    NewStringType::kNormal).ToLocalChecked()));
    }
}

/* Run a script and return the resulting "exports" object */
v8::MaybeLocal<Value> execute_as_module(client_connection *cc,v8::Local<v8::Script> script) noexcept {
    using namespace v8;

    auto context = cc->new_user_context(true).second;
    Context::Scope cscope{context};

    Local<String> str_exports;
    bool has;
    if(
        !cc->get_str(client_connection::STR_EXPORTS).ToLocal(&str_exports) ||
        context->Global()->Set(
            context,
            str_exports,
            Object::New(cc->isolate)).IsNothing() ||
        script->Run(context).IsEmpty() ||
        !context->Global()->Has(context,str_exports).To(&has)) return {};

    if(has) return context->Global()->Get(context,str_exports);
    return Object::New(cc->isolate);
}

// this is for when x is supposed to be one of our JS functions
v8::MaybeLocal<v8::Function> to_function(v8::Isolate isolate,v8::Local<v8::Value> x) {
    if(!x->IsFunction()) {
        isolate->ThrowException(
            v8::Exception::TypeError(
                v8::String::NewFromUtf8(
                    isolate,
                    "core function missing",
                    v8::NewStringType::kNormal).ToLocalChecked()));
        return {};
    }
    return v8::Local<v8::Function>::Cast(x);
}

bool feed_base64_encoded(WasmModuleObjectBuilderStreaming &builder,const char *data,size_t size) {
    static constexpr char pad_char = '=';
    static constexpr size_t buf_size = 300; // must be divisable by 3
    uint8_t buffer[buf_size];

    if(p != buffer) builder.OnBytesReceived(buffer,p - buffer);

    if(size % 4) return false;

    uint8_t *dest = buffer;

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
                    return false;
                }
            } else return false;
            ++data;
        }
        *dest++ = (temp >> 16) & 0xFF;
        *dest++ = (temp >> 8) & 0xFF;
        *dest++ = (temp) & 0xFF;

        if(dest == buffer + buf_size) {
            builder.OnBytesReceived(buffer,buf_size);
            dest = buffer;
        }
    }

end:
    if(dest != buffer) builder.OnBytesReceived(buffer,dest - buffer);
    return true;
}

struct wasm_compile_data {
    std::shared_ptr<std::string> name;
    program_t::module_cache_map::iterator itr;
};
void wasm_compile_success(const v8::FunctionCallbackInfo<v8::Value> &args) noexcept {
    using namespace v8;

    if(args.Length() < 1) return;

    auto isolate = args.GetIsolate();
    HandleScope scope(isolate);

    auto cc = reinterpret_cast<client_connection*>(isolate->GetData(0));
    auto context = isolate->GetCurrentContext();

    if(!args[0].IsWebAssemblyCompiledModule())
        isolate->ThrowException(
            Exception::TypeError(
                String::NewFromUtf8(
                    isolate,
                    "expected web assembly module",
                    NewStringType::kNormal).ToLocalChecked()));

    auto mod = WasmCompiledModule::Cast(args[0]);
    cached_module cache(mod->GetTransferrableModule());

    Local<Object> vals;
    Local<Value> name;
    Local<Value> mid;
    if(!(args.Data().ToObject(context).ToLocal(&vals) &&
        vals.Get(context,0).ToLocal(&name) &&
        vals.Get(context,1).ToLocal(&mid))) return;

    {
        uvwrap::mutex_scope_lock lock{program->module_cache_lock};
        auto itr = program->module_cache.find(std_id);
        if(itr == program->module_cache.end()) {
            isolate->ThrowException(
                Exception::Error(
                    String::NewFromUtf8(
                        isolate,
                        "missing module data",
                        NewStringType::kNormal).ToLocalChecked()));
            return;
        }
        std::swap(*itr,cache);
    }

    Local<String> func_name;
    Local<Value> func_val;
    Local<Function> func;
    Local<Value> args[] = {name,mid,mod};
    if(!(cc->try_get_str(RESOLVE_READY_MODULE).ToLocal(&func_name) &&
        context->Global()->Get(context,func_name).ToLocal(&func_val) &&
        to_function(isolate,func_val).ToLocal(&func) &&
        func->Call(context,
            context->Global(),
            sizeof(args)/sizeof(Local<Value>),
            args))) return;
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

    String::Utf8Value id{isolate,args.Data()};
    if(!*id) return;
    v8::String::Utf8Value name{isolate,args[0]};
    if(!*name) return;

    try {
        std::string std_id{*id,static_cast<size_t>(id.length())};

        program_t::module_cache_map::iterator itr;
        {
            uvwrap::mutex_scope_lock lock{program->module_cache_lock};
            itr = program->module_cache.find(std_id);
            if(itr == program->module_cache.end()) {
                isolate->ThrowException(
                    Exception::Error(
                        String::NewFromUtf8(
                            isolate,
                            "missing module data",
                            NewStringType::kNormal).ToLocalChecked()));
                return;
            }

            if(itr->second.tag == cached_module::UNCOMPILED_WASM ||
                itr->second.tag == cached_module::UNCOMPILED_JS) {
                if(itr->second.data.buff.started) {
                    /* another thread is compiling this module, wait for it to
                    finish */
                    itr->second.data.buff.waiting.emplace_back(
                        std::string{*name,size_t(name.length())}
                        cc->connection_id,
                        client_connection::get_context_id(context));

                    args.GetReturnValue().SetNull();
                    return;
                }

                itr->second.data.buff.started = true;
            }
        }

        switch(itr->second.tag) {
        case cached_module::WASM:
            {
                Local<WasmCompiledModule> r;
                if(WasmCompiledModule::FromTransferrableModule(isolate,itr->second.data.wasm).ToLocal(&r))
                    args.GetReturnValue().Set(r);
            }
            break;
        case cached_module::JS:
            {
                auto &cache = itr->second.data.js_cache;

                Local<String> script_str, str_string;
                if(!(String::NewFromUtf8(
                        isolate,
                        cache.code.data(),
                        NewStringType::kNormal,
                        cache.code.size()).ToLocal(&script_str) &&
                    cc->get_str(client_connection::STR_STRING).ToLocal(&str_string))) return;

                ScriptCompiler::CachedData data{cache.data,cache.length};
                Source source{
                    script_str,
                    ScriptOrigin{str_string},
                    &data};
                Local<Script> script;
                Local<Value> r;
                if(!(
                    ScriptCompiler::Compile(
                        context,
                        &source,
                        ScriptCompiler::kConsumeCodeCache).ToLocal(&script) &&
                    execute_as_module(cc,script).ToLocal(&r))) return;
                args.GetReturnValue().Set(r);
            }
            break;
        case cached_module::UNCOMPILED_WASM:
            {
                auto &buff = itr->second.data.buff;
                auto contents = buf->GetContents();
                WasmModuleObjectBuilderStreaming builder{client->isolate};

                {
                    auto promise = builder.GetPromise();
                    auto vals = Array::New(isolate,2);
                    Local<Function> tmp_fthen;
                    Local<Function> tmp_fcatch;
                    Local<Promise> tmp_p;
                    if(vals.Set(context,0,args[0]).IsNothing() ||
                        vals.Set(context,1,args.Data()).IsNothing() ||
                        !(Function::New(context,&wasm_compile_success,vals).ToLocal(&tmp_fthen) &&
                        Function::New(context,&wasm_compile_fail,vals).ToLocal(&tmp_fcatch) &&
                        promise->Then(context,tmp_fthen) &&
                        !tmp_p->Catch(context,tmp_fcatch))) return;
                }

                if(!feed_base64_encoded(builder,buff.data.get(),buf.length)) {
                    isolate->ThrowException(
                        Exception::Error(
                            String::NewFromUtf8(
                                isolate,
                                "invalid base64 encoded data",
                                NewStringType::kNormal).ToLocalChecked()));
                    return;
                }
                builder.Finish();

                args.GetReturnValue().SetNull();
                return;
            }
            break;
        default:
            assert(itr->second.tag == cached_module::UNCOMPILED_JS);
            {
                auto &buff = itr->second.data.buff;
                Local<String> script_str, str_string;
                if(!(String::NewFromUtf8(
                        isolate,
                        buff.data.get(),
                        NewStringType::kNormal,
                        buff.length).ToLocal(&script_str) &&
                    cc->get_str(client_connection::STR_STRING).ToLocal(&str_string))) goto compile_error;

                Source source{
                    script_str,
                    ScriptOrigin{str_string}};
                Local<Script> script;
                if(!ScriptCompiler::Compile(
                    context,
                    &source,
                    ScriptCompiler::kProduceCodeCache).ToLocal(&script)) goto compile_error;
                auto cache = source.GetCachedData();
                auto mod = cache ?
                    cached_module{to_std_str(script_str),cache->data,cache->length} :
                    cached_module{to_std_str(script_str),nullptr,0};

                {
                    uvwrap::mutex_scope_lock lock{module_cache_lock};
                    std::swap(mod,*itr);
                }

                for(auto &w : mod.data.buff.waiting) {
                    auto w_cc = program->get_connection(w.cc_id);
                    if(w_cc) w_cc->register_other_task<module_task>(w.name,std_id,w.con_id);
                }

                Local<Value> r;
                if(!execute_as_module(cc,script).ToLocal(&r)) return;
                args.GetReturnValue().Set(r);
            }
            break;
        }

        return;

    compile_error:
        uvwrap::mutex_scope_lock lock{module_cache_lock};

        std::vector<waiting_mod_req> waiting;
        std::swap(waiting,mod.data.buff.waiting);

        if(cc->closed) {
            // let another thread compile the module
            itr->second.data.buff.started = false;

            for(auto &w : waiting) {
                auto w_cc = program->get_connection(w.cc_id);
                if(w_cc) w_cc->register_other_task<module_task>(w.name,std_id,w.con_id);
            }
        } else {
            for(auto &w : waiting) {
                auto w_cc = program->get_connection(w.cc_id);
                if(w_cc) w_cc->register_other_task<module_error_task>(w.name,std_id,w.con_id);
            }
        }
    } catch(std::exception &e) {
        isolate->ThrowException(
            Exception::Error(
                String::NewFromUtf8(
                    isolate,
                    e.what(),
                    NewStringType::kNormal).ToLocalChecked()));
    }
}

std::pair<unsigned long,v8::Local<v8::Context>> get_context(client_connection *cc,v8::Local<v8::Object> msg) {
    v8::Local<v8::Value> js_id = check_r(msg->Get(cc->get_str(client_connection::STR_CONTEXT)));
    if(js_id->IsNullOrUndefined())
        return cc->new_user_context(true);

    int64_t id = check_r(js_id->ToInteger(cc->request_context.Get(cc->isolate)))->Value();
    if(id < 0) throw bad_context{};
    auto itr = cc->contexts.find(static_cast<unsigned long>(id));
    if(itr == cc->contexts.end()) throw bad_context{};
    return std::make_pair(itr->first,itr->second.Get(cc->isolate));
}

std::vector<v8::Local<v8::Value>> get_array_items(v8::Local<v8::Array> x) {
    std::vector<v8::Local<v8::Value>> r;
    uint32_t length = x->Length();
    r.reserve(length);
    for(uint32_t i=0; i<length; ++i) r.push_back(check_r(x->Get(i)));
    return r;
}

#define CACHED_STR(X) (client->get_str(client_connection::STR_##X))

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
                Local<String> code = check_r(get_value(str_code,request_context))->ToString(request_context));
                auto context = get_context(client,msg);
                Context::Scope context_scope(context.second);

                Local<Script> compiled;
                Local<Value> result;
                Local<String> json_result;
                if(Script::Compile(context.second,code).ToLocal(&compiled) &&
                        compiled->Run(context.second).ToLocal(&result) &&
                        JSON::Stringify(context.second,result).ToLocal(&json_result)) {
                    response_builder b{client->isolate};
                    b("type","\"result\"");
                    if(context.first) b("context",context.first);
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
                auto context = get_context(client,msg);
                Context::Scope context_scope(context.second);

                Local<Script> compiled;
                if(Script::Compile(context.second,code).ToLocal(&compiled) && !compiled->Run(context.second).IsEmpty()) {
                    response_builder b{client->isolate};
                    b("type","\"success\"");
                    if(context.first) b("context",context.first);
                    b.send(stream);
                } else if(!client->closed) {
                    queue_script_exception(stream,trycatch,client->isolate,context.second);
                }
            }
            break;
        case CALL:
            {
                Local<String> fname = check_r(get_value(str_func,request_context)
                    ->ToString(request_context));
                Local<Value> tmp = get_value(str_args,request_context);

                if(tmp->IsArray()) {
                    Local<Array> args = Local<Array>::Cast(tmp);
                    auto context = get_context(client,msg);
                    Context::Scope context_scope(context.second);

                    tmp = check_r(context.second->Global()->Get(context.second,fname));
                    if(tmp->IsFunction()) {
                        Local<Function> func = Local<Function>::Cast(tmp);
                        Local<Value> result;
                        Local<String> json_result;
                        auto args_v = get_array_items(args);
                        if(func->Call(context.second,context.second->Global(),static_cast<int>(args_v.size()),args_v.data()).ToLocal(&result) &&
                                JSON::Stringify(context.second,result).ToLocal(&json_result)) {
                            response_builder b{client->isolate};
                            b("type","\"result\"");
                            b("value",*json_result);
                            if(context.first) b("context",context.first);
                            b.send(stream);
                        } else if(!client->closed) {
                            queue_script_exception(stream,trycatch,client->isolate,context.second);
                        }
                    } else {
                        string_builder b;
                        b.add_string(R"({"type":"error","errtype":"request","message":")");
                        String::Value str_val{client->isolate,fname};
                        if(!*str_val) throw bad_request{};
                        b.add_escaped();
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
                b.add_integer(client_connection::conn_count.load());
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
    } catch(bad_request&) {
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
            isolate,
            name.data(),
            v8::NewStringType::kNormal,
            name.size()).ToLocal(&js_name))
        {
            fprintf(stderr,"failed to load module name\n");
            cc->close_async();
            return;
        }

        TryCatch trycatch{cc->isolate};
        Local<Value> js_mid = Null(isolate);
        try {
            js_mid = from_std_str(client->isolate,mid);
            Local<Value> args[] = {
                js_name,
                js_mid,
                check_r(Function::New(context,&getCachedMod,js_mid))
            };
            check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(RESOLVE_MODULE))))->Call(
                context,
                context->Global(),
                sizeof(args)/sizeof(Local<Value>),
                args));
        } catch(bad_request&) {
            if(client->closed || !trycatch.CanContinue()) return;
            if(trycatch.HasTerminated()) client->isolate->CancelTerminateExecution();
            try {
                Local<Value> args[] = {
                    js_name,
                    js_mid,
                    check_r(trycatch.Exception().ToString(context))
                };
                check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(FAIL_MODULE))))->Call(
                    context,
                    context->Global(),
                    sizeof(args)/sizeof(Local<Value>),
                    args));
            } catch(bad_request&) {
                if(client->closed) return;
                fprintf(stderr,"error occurred while trying to report another error\n");
                cc->close_async();
            }
        }
    } catch(...) {
        fprintf(stderr,"unexpected error type\n");
        cc->close_async();
    }
}

void module_error_task::Run() {
    using namespace v8;

    if(client->closed) return;

    try {
        auto itr = cc->contexts.find(context_id);
        if(itr == cc->contexts.end()) {
            /* the context has been closed, so we are done here */
            return;
        }
        auto context = itr->second.Get(client->isolate);
        Context::Scope context_scope(context);

        auto js_mid = from_std_str(client->isolate,mid);
        Local<Value> args[] = {
            from_std_str(client->isolate,name),
            from_std_str(client->isolate,msg)
        };
        check_r(to_function(client->isolate,check_r(context->Global()->Get(context,CACHED_STR(FAIL_MODULE))))->Call(
            context,
            context->Global(),
            sizeof(args)/sizeof(Local<Value>),
            args));
    } catch(bad_request&) {
        if(!client->closed)
            fprintf(stderr,"error occurred while trying to report another error\n");
        cc->close_async();
    } catch(...) {
        fprintf(stderr,"unexpected error type\n");
        cc->close_async();
    }
}

#undef CACHED_STR

void program_t::register_task() {
    almost_json_parser::value_map values;
    std::swap(values,command_buffer.value);
    unsigned long long cc_id;
    unsigned long context_id;

    try {
        cc_id = std::stoull(get_str_value(this,request_task::str_connection_id));
        context_id = std::stoull(get_str_value(this,request_task::str_context_id));
    } catch(...) {
        queue_response(
            stdout,
            copy_string(R"({"type":"error","errtype":"request","message":"invalid connection or context id"})"));
        return;
    }

    std::shared_ptr<v8::Task> task;

    switch(type) {
    case MODULE:
        /* Stash the module data. It will be compiled by one of the
        connections's threads */
        {
            auto &name = get_str_value(this,request_task::str_name);
            auto &mid = get_str_value(this,request_task::str_id);
            auto &mtype = get_str_value(this,request_task::str_modtype);
            auto &data = get_str_value(this,request_task::str_value);

            cached_module::tag_t tag;
            if(mtype == "js") tag = cachd_module::UNCOMPILED_JS;
            else if(mtype == "wasm") tag = cached_module::UNCOMPILED_WASM;
            else {
                queue_response(
                    stdout,
                    copy_string(R"({"type":"error","errtype":"request","message":"invalid module type"})"));
                break;
            }

            {
                uvwrap::mutex_scope_lock lock{module_cache_lock};
                module_cache.emplace(mid,data.data(),data.size(),tag);
            }

            task.reset(new module_task(name,mid,context_id));
        }
        break;
    case MODULE_CACHED:
        task.reset(new module_task(
            get_str_value(this,request_task::str_name),
            get_str_value(this,request_task::str_id),
            context_id));
        break;
    default:
        assert(type == MODULE_ERROR);
        task.reset(new module_error_task(
            get_str_value(this,request_task::str_name),
            get_str_value(this,request_task::str_msg),
            context_id));
        break;
    }

    auto cc = get_connection(cc_id);

    if(cc) {
        cc->tloop.add_task(task);
        queue_response(
            stdout,
            copy_string(R"({"type":"success","connectionalive":"true"})"));
    } else {
        /* The connection no longer exists, but we keep the module data;
        another connection can request it. */
        queue_response(
            stdout,
            copy_string(R"({"type":"success","connectionalive":"false"})"));
    }
}

void on_new_connection(uv_stream_t *server_,int status) noexcept {
    if(status < 0) {
        fprintf(stderr,"connection error: %s\n",uv_strerror(status));
        return;
    }

    try {
        auto cc = new client_connection;

        ASSERT_UV_RESULT(uv_accept(server_,reinterpret_cast<uv_stream_t*>(&cc->client.data)));

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
    fprintf(stderr,"shutting down\n");
    program->sig_request.stop(std::nothrow);
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

program_t::program_t(const char *progname) :
    v8_program_t{progname},
    server{main_loop},
    sig_request{main_loop},
    stdin{main_loop},
    stdout{main_loop},
    func_dispatcher{
        main_loop,
        [](uv_async_t *handle) {
            reinterpret_cast<program_t*>(handle->data)->run_functions();
        },
        this
    },
    snapshot{create_snapshot()},
    next_conn_index{0}
{
    if(!snapshot.data) {
        throw std::runtime_error("failed to create start-up snapshot");
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

    try {
        program_t p{argv[0]};
        program = &p;

        p.sig_request.start(&signal_handler,SIGINT);
        p.server.bind(argv[1]);
        p.server.listen(128,&on_new_connection);
        p.stdin.open(0);
        p.stdin.read_start([](uv_stream_t *s,ssize_t read,const uv_buf_t *buf) noexcept {
            auto p = reinterpret_cast<program_t*>(s->data);
            p->handle_input(reinterpret_cast<uv_stream_t*>(&p->stdout.data),read,buf);
        });
        p.stdout.open(1);

        /* this is to let a parent process know that a socket/pipe is ready */
        printf("ready\n");
        fflush(stdout);

        p.main_loop.run(UV_RUN_DEFAULT);
    } catch(std::exception &e) {
        fprintf(stderr,"%s\n",e.what());
        return 2;
    }

    return 0;
}

