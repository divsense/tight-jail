#include <libplatform/libplatform.h>
#include <v8.h>
#include <uv.h>
#include <memory>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>
#include <atomic>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "almost_json_parser.hpp"


#if V8_MAJOR_VERSION >= 7
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length(isolate))
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(isolate,data))
#else
#  define STRING_UTF8LENGTH(str,isolate) ((str)->Utf8Length())
#  define STRING_WRITEUTF8(str,isolate,data) ((str)->WriteUtf8(data))
#endif

#ifdef NDEBUG
#  define ASSERT_UV_RESULT(X) (X)
#else
#  define ASSERT_UV_RESULT(X) assert((X) == 0)
#endif

#define RESOLVE_MODULE_FUNC_NAME "__resolveModule"
#define RESOLVE_MODULE_CACHE_FUNC_NAME "__resolveModuleFromCache"
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

var __moduleCache = {};

function jimport(m) {
  if(r instanceof __PendingMod) return r.promise;
  if(r !== undefined) return Promise.resolve(r);

  return new Promise((resolve,reject) => {
    let r = __moduleCache[m];
    if(r === null) reject(new JailImportError('module not found'));
    else if(r instanceof __PendingMod) r.add([resolve,reject]);
    else if(r === undefined) {
      __moduleCache[m] = new __PendingMod(resolve,reject);
      __sendImportMessage(m);
    }
    else resolve(r);
  });

  return r;
}

function __resolveModule(name,normName,m) {
    let pending = __moduleCache[name];
}

function __resolveModuleFromCache(name,normName) {
  let pending = __moduleCache[name];
  let r = __moduleCache[normName];
  if(r isntanceof __PendingMod) {
    if(pending !== r) {
      r.mergeFrom(pending);
      __moduleCache[name] = r;
    }
  } else if(r === undefined) {
    r = __instantiateModuleFromCache(normName);
    __moduleCache[name] = r;
    __moduleCache[normName] = r;
    pending.resolve(r);
  } else {
    __moduleCache[name] = r;
    pending.resolve(r);
  }
}

function __failModule(name,message) {
  let pending = __PendingMod[name];
  delete __PendingMod[name];
  pending.reject(new JailImportError(message));
}
)";

uv_loop_t *main_loop;
uv_pipe_t server;
uv_signal_t sig_request;

v8::Isolate::CreateParams create_params;
v8::Platform *platform;

uv_rwlock_t module_cache_lock;
struct js_cache {
    std::string code;
    std::unique_ptr<uint8_t[]> data;
    int length;
};
struct cached_module {
    enum {WASM,JS} tag;
    union {
        v8::WasmCompiledModule::TransferrableModule wasm;
        js_cache js;
    } data;

    cached_module(v8::WasmCompiledModule::TransferrableModule &&wasm) : tag{WASM} {
        new(&data.wasm) v8::WasmCompiledModule::TransferrableModule(std::move(wasm));
    }

    cached_module(std::string code,uint8_t *data,int length) : tag{JS} {
        new(&data.js) js_cache({code,std::unique_ptr<uint8_t[]>{new uint8_t[length]},length});
        memcpy(data.js.data.get(),data,length);
    }

    ~cached_module() {
        if(tag == WASM) data.wasm.~TransferrableModule();
        else data.js.~js_cache();
    }
};
std::map<std::string,cached_module> module_cache;

struct bad_request {};
struct bad_context {};
struct uv_exception { int code; };

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
    return std::string{*id,id.length()};
}

class mutex_scope_lock {
    uv_mutex_t &m;

public:
    mutex_scope_lock(uv_mutex_t &m) : m(m) {
        uv_mutex_lock(&m);
    }

    ~mutex_scope_lock() {
        uv_mutex_unlock(&m);
    }
};

class read_scope_lock {
    uv_rwlock_t &rw;

public:
    read_scope_lock(uv_rwlock_t &rw) : rw(rw) {
        uv_rwlock_rdlock(&rw);
    }

    ~read_scope_lock() {
        uv_rwlock_rdunlock(&rw);
    }
};

class write_scope_lock {
    uv_rwlock_t &rw;

public:
    write_scope_lock(uv_rwlock_t &rw) : rw(rw) {
        uv_rwlock_wrlock(&rw);
    }

    ~write_scope_lock() {
        uv_rwlock_wrunlock(&rw);
    }
};

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

struct request_task;

struct task_loop {
    v8::Isolate *isolate;
    uv_loop_t loop;
    uv_async_t dispatcher;
    uv_thread_t thread;
    std::shared_ptr<v8::TaskRunner> task_runner;
    volatile bool closing = false;

    task_loop(v8::Isolate *isolate) : isolate{isolate}, task_runner{platform->GetForegroundTaskRunner(isolate)} {
        int r;
        if((r = uv_loop_init(&loop))) throw uv_exception{r};
        try {
            if((r = uv_async_init(
                &loop,
                &dispatcher,
                [](uv_async_t *handle) {
                    auto tloop = reinterpret_cast<task_loop*>(handle->data);
                    if(tloop->closing) {
                        uv_stop(&tloop->loop);
                    } else {
                        while(v8::platform::PumpMessageLoop(platform,tloop->isolate))
                            tloop->isolate->RunMicrotasks();
                    }
                })))
                throw uv_exception{r};

            dispatcher.data = this;

            if((r = uv_thread_create(
                &thread,
                [](void *arg) {
                    uv_run(reinterpret_cast<uv_loop_t*>(arg),UV_RUN_DEFAULT);
                },
                &loop)))
            {
                uv_close(reinterpret_cast<uv_handle_t*>(&dispatcher),nullptr);
                throw uv_exception{r};
            }
        } catch(...) {
            uv_run(&loop,UV_RUN_DEFAULT); // needed to free any handles
            throw;
        }
    }

    ~task_loop() {
        closing = true;
        ASSERT_UV_RESULT(uv_async_send(&dispatcher));
        ASSERT_UV_RESULT(uv_thread_join(&thread));
        closing = false; // don't try to stop the loop again

        uv_close(reinterpret_cast<uv_handle_t*>(&dispatcher),nullptr);

        uv_run(&loop,UV_RUN_DEFAULT); // needed to free any handles

        ASSERT_UV_RESULT(uv_loop_close(&loop));
    }

    void add_task(std::unique_ptr<v8::Task> &&task) {
        task_runner->PostTask(std::move(task));
        ASSERT_UV_RESULT(uv_async_send(&dispatcher));
    }
};

/* a seperate struct so that "isolate->Dispose()" gets called even if the
constructor for client_connection throws */
struct autoclose_isolate {
    v8::Isolate* isolate;

    autoclose_isolate(v8::Isolate* isolate) : isolate{isolate} {}
    ~autoclose_isolate() { isolate->Dispose(); }
};

/* The lifetime management for this is a little unusual. When "close()" is
 * called, it schedules a callback to delete itself, but if there are still any
 * request instances that belong to this, by the time the callback is called,
 * it won't delete itself and instead the last request to be destroyed will
 * delete this. */
struct client_connection : private list_node, public autoclose_isolate {
    typedef std::map<unsigned long,v8::UniquePersistent<v8::Context>> context_map;

    static client_connection *conn_head;

    // used for "getstats"
    static std::atomic<unsigned long> conn_count;

    uv_pipe_t client;

    task_loop tloop;
    almost_json_parser::parser command_buffer;
    v8::UniquePersistent<v8::Context> request_context;
    request_task *requests_head;
    context_map contexts;
    unsigned long next_id;

    v8::UniquePersistent<v8::ObjectTemplate> globals_template;

    /* when true, the last request to reference this connection will delete this
     */
    bool closed;

    /* when true, there was a parse error in current request and further input
    will be ignored until the start of the next request */
    bool parse_error;

    enum {
        STR_TYPE=0,
        STR_FUNC,
        STR_ARGS,
        STR_CONTEXT,
        STR_URI,
        STR_CODE,
        STR_CREATE_CONTEXT,
        STR_DESTROY_CONTEXT,
        STR_EVAL,
        STR_EXEC,
        STR_CALL,
        STR_GETSTATS,
        COUNT_STR
    };

    v8::UniquePersistent<v8::String> str[COUNT_STR];
    static const char *str_raw[COUNT_STR];

    client_connection() :
            autoclose_isolate{v8::Isolate::New(create_params)},
            tloop{isolate},
            requests_head{nullptr},
            next_id{1},
            closed{false},
            parse_error{false} {
        int r;
        if((r = uv_pipe_init(main_loop,&client,0))) throw uv_exception{r};

        client.data = this;

        isolate->SetData(0,this);

        if(conn_head) conn_head->insert_before(this);
        conn_head = this;
        ++conn_count;
    }

    ~client_connection() {
        assert(!requests_head && closed);
        --conn_count;
        if(this == conn_head) conn_head = static_cast<client_connection*>(next);
        remove();

        /* all the JS references need to be freed before calling
           isolate->Dispose */
        for(auto &item : str) item.Reset();
        request_context.Reset();
        globals_template.Reset();
        contexts.clear();
    }

    v8::Local<v8::ObjectTemplate> get_globals();

    v8::Local<v8::String> get_str(unsigned int i);

    void close();

    void register_task();

    static void close_all() {
        for(auto itr = conn_head; itr; itr = static_cast<client_connection*>(itr->next)) itr->close();
    }
};

client_connection *client_connection::conn_head = nullptr;
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

v8::Local<v8::String> client_connection::get_str(unsigned int i) {
    assert(i < COUNT_STR);
    if(str[i].IsEmpty()) {
        str[i].Reset(isolate,check_r(v8::String::NewFromUtf8(
            isolate,
            str_raw[i],
            v8::NewStringType::kNormal)));
    }
    return str[i].Get(isolate);
}

const char *client_connection::str_raw[client_connection::COUNT_STR] = {
    "type",
    "func",
    "args",
    "context",
    "URI",
    "code",
    "createcontext",
    "destroycontext",
    "eval",
    "exec",
    "call",
    "getstats"
};

struct write_request {
    uv_write_t req;
    std::unique_ptr<char[]> msg;

    write_request(char *msg) : msg(msg) {
        req.data = this;
    }
};

struct request_task : private list_node, public v8::Task {
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

    client_connection *client;
    almost_json_parser::value_map values;
    bool cancelled;

    ~request_task() {
        if(client->requests_head == this) client->requests_head = static_cast<request_task*>(next);
        remove();
        if(!client->requests_head && client->closed) delete client;
    }

    void Run() override;

    v8::Local<v8::Value> get_value(const std::string &name,v8::Local<v8::Context> context) const;

    void cancel() {
        cancelled = true;
    }

private:
    request_task(client_connection *client,type_t type) :
        type{type_t},
        client{client},
        cancelled(false) {}
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

v8::Local<v8::Value> request_task::get_value(const std::string &name,v8::Local<v8::Context> context) const {
    auto itr = values.find(name);
    if(itr == values.end()) {
        client->isolate->ThrowException(
            v8::Exception::TypeError(
                v8::String::NewFromUtf8(
                    isolate,
                    "invalid request",
                    v8::NewStringType::kNormal).ToLocalChecked()));
        throw bad_request{};
    }

    auto r = check_r(String::NewFromUtf8(
        client->isolate,
        itr->data.data(),
        NewStringType::kNormal,
        itr->data.size()));
    return itr->is_string ? r : check_r(JSON::Parse(context,r));
}

void client_connection::register_task() {
    auto type = command_buffer.value.find(request_task::str_type);
    if(!type.is_string) {
        queue_response(
            reinterpret_cast<uv_stream_t*>(client),
            copy_string(R"({"type":"error","errtype":"request","message":"invalid request"})"));
        return;
    }

    request_task::type_t etype;
    for(int i=0; i<request_task::COUNT_TYPES; ++i) {
        if(request_task::request_types[i] == type.data) {
            etype = static_cast<request_type::type_t>(i);
            goto found_one;
        }
    }

    queue_response(
        reinterpret_cast<uv_stream_t*>(client),
        copy_string(R"({"type":"error","errtype":"request","message":"unknown request type"})"));
    return;

found_one:
    auto task = new request_task(this,etype);
    std::swap(task->values,command_buffer.value);
    if(requests_head) requests_head->insert_before(task);
    requests_head = task;
    tloop.add_task(std::unique_ptr<request_task>{task});
}

void client_connection::close() {
    if(!closed) {
        closed = true;
        isolate->TerminateExecution();

        uv_close(
            reinterpret_cast<uv_handle_t*>(&client),
            [](uv_handle_t *h) {
                auto self = reinterpret_cast<client_connection*>(h->data);
                delete self;
            });
    }
}

void queue_response(uv_stream_t *stream,char *msg,ssize_t len=-1) {
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

char hex_digit(int x) {
    assert(x >= 0 && x < 16);
    return "01234567890abcdef"[x];
}

class string_builder {
    std::vector<char> buffer;

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
    for(int i = 0; i < str.length(); i++) {
        uint16_t c = (*str)[i];
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

template<size_t N> char *copy_string(const char (&str)[N]) {
    char *r = new char[N];
    memcpy(r,str,N);
    return r;
}

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

void sendImportMessage(const v8::FunctionCallbackInfo<v8::Value> &args) {
    if(args.Length() < 1) return;

    auto isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);

    auto context = isolate->GetCurrentContext();
    auto stream = reinterpret_cast<uv_stream_t*>(
        &reinterpret_cast<client_connection*>(isolate->GetData(0))->client);

    v8::String::Value mname{isolate,args[0]}
    if(!*mname) return;

    string_builder tmp;
    tmp.add_string(R"({"type":"import","name":")");
    tmp.add_escaped(mname);
    tmp.add_string("\"}");

    queue_response(stream,tmp.get_string());
}

void getCachedMod(const v8::FunctionCallbackInfo<v8::Value> &args) {
    using namespace v8;

    auto isolate = args.GetIsolate();
    HandleScope scope(isolate);

    auto context = isolate->GetCurrentContext();

    String::Utf8Value id{isolate,args.Data()};
    if(!*id) return;

    read_scope_lock lock{module_cache_lock};
    auto itr = module_cache.find(std::string{*id,id.length()});
    if(itr == module_cache.end()) {
        args.GetReturnValue().SetNull();
        return;
    }

    Local<WasmCompiledModule> r;
    if(WasmCompiledModule::FromTransferrableModule(isolate,*itr).ToLocal(&r))
        args.GetReturnValue().Set(r);
}

std::pair<unsigned long,v8::Local<v8::Context>> get_context(client_connection *cc,v8::Local<v8::Object> msg) {
    v8::Local<v8::Value> js_id = check_r(msg->Get(cc->get_str(client_connection::STR_CONTEXT)));
    if(js_id->IsNullOrUndefined())
        return std::make_pair(
            0,
            v8::Context::New(cc->isolate,nullptr,cc->get_globals()));

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

// this is for when x is supposed to be one of our JS functions
v8::Local<v8::Function> to_function(v8::Isolate isolate,v8::Local<Value> x) {
    if(!x->IsFunction()) {
        isolate->ThrowException(
            v8::Exception::TypeError(
                v8::String::NewFromUtf8(
                    isolate,
                    "core function missing",
                    v8::NewStringType::kNormal).ToLocalChecked()));
        throw bad_request{};
    }
    return v8::Local<v8::Function>::Cast(x);
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
    Context::Scope req_context_scope(request_context);

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
                Local<String> code = check_r(check_r(
                    msg->Get(request_context,CACHED_STR(CODE)))->ToString(request_context));
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
                Local<String> fname = check_r(check_r(
                    msg->Get(request_context,CACHED_STR(FUNC)))->ToString(request_context));
                Local<Value> tmp = check_r(
                    msg->Get(request_context,CACHED_STR(ARGS)));

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
                unsigned long id = client->next_id++;
                client->contexts.emplace(
                    id,
                    UniquePersistent<Context>{
                        client->isolate,
                        Context::New(client->isolate,nullptr,client->get_globals())});

                response_builder{client->isolate}
                    ("type","\"success\"")
                    ("context",id).send(stream);
            }
            break;
        case DESTROY_CONTEXT:
            {
                int64_t id = check_r(check_r(msg->Get(request_context,CACHED_STR(CONTEXT)))
                    ->ToInteger(client->request_context.Get(client->isolate)))->Value();
                if(id < 0) throw bad_context{};
                auto itr = client->contexts.find(static_cast<unsigned long>(id));
                if(itr == client->contexts.end()) throw bad_context{};
                client->contexts.erase(itr);

                response_builder{client->isolate}
                    ("type","\"success\"")
                    ("context",id).send(stream);
            }
            break;
        case MODULE:
            {
                try {
                    Local<String> mname = check_r(check_r(msg->Get(request_context,CACHED_STR(NAME)))->ToString(request_context));
                    Local<String> mid = check_r(check_r(msg->Get(request_context,CACHED_STR(NORM_NAME)))->ToString(request_context));
                    Local<Value> value = check_r(msg->Get(request_context,CACHED_STR(VALUE)));

                    auto context = get_context(client,msg);
                    Context::Scope context_scope(context.second);

                    if(value->IsArrayBuffer()) {
                        auto buf = Local<ArrayBuffer>::Cast(value);
                        auto contents = buf->GetContents();
                        WasmModuleObjectBuilderStreaming builder{client->isolate};
                        auto promise = builder.GetPromise();
                        builder.OnBytesReceived(reinterpret_cast<const uint8_t*>(contents.Data()),contents.ByteLength());
                        builder.Finish();
                    } else {
                        auto script_str = check_r(value->ToString(request_context));
                        Source source{
                            script_str,
                            ScriptOrigin{CACHED_STR(STRING)};
                        auto script = ScriptCompiler::Compile(
                            context,
                            &source,
                            ScriptCompiler::kProduceCodeCache);
                        auto cache = source.GetCachedData();
                        if(cache) {
                            try {
                                write_scope_lock lock{module_cache_lock};
                                module_cache.emplace(
                                    to_std_str(client->isolate,mid),
                                    to_std_str(client->isolate,script_str),
                                    cache->data,
                                    cache->length);
                            } catch(std::exception &e) {
                                fprintf(stderr,"failed to add to cache, %s\n",e.what());
                            }
                        }

                        value = script;

                        Local<Value> args[] = {mname,mid,value};
                        check_r(to_function(client->isolate,check_r(context.second->Global()->Get(context.second,CACHED_STR(RESOLVE_MODULE))))->Call(
                            context.second,
                            context.second->Global(),
                            sizeof(args)/sizeof(Local<Value>),
                            args));
                    }
                } catch(bad_request&) {
                } catch(...) {
                    fprintf(stderr,"unexpected error type\n");
                }
            }
            break;
        case MODULE_CACHED:
            {
                try {
                    Local<String> mname = check_r(check_r(msg->Get(request_context,CACHED_STR(NAME)))->ToString(request_context));
                    Local<String> mid = check_r(check_r(msg->Get(request_context,CACHED_STR(NORM_NAME)))->ToString(request_context));

                    auto context = get_context(client,msg);
                    Context::Scope context_scope(context.second);

                    {
                        Local<Value> args[] = {
                            mname,
                            mid,
                            check_r(Function::New(context,&getCachedMod,mid))
                        };
                        check_r(to_function(client->isolate,check_r(context.second->Global()->Get(context.second,CACHED_STR(RESOLVE_MODULE_CACHE))))->Call(
                            context.second,
                            context.second->Global(),
                            sizeof(args)/sizeof(Local<Value>),
                            args));
                    }
                } catch(bad_request&) {
                } catch(...) {
                    fprintf(stderr,"unexpected error type\n");
                }
            }
            break;
        case MODULE_ERROR:
            {
                Local<String> mname;
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

#undef CACHED_STR

void on_new_connection(uv_stream_t *server_,int status) noexcept {
    if(status < 0) {
        fprintf(stderr,"connection error: %s\n",uv_strerror(status));
        return;
    }

    client_connection *cc;
    try {
        cc = new client_connection;
    } catch(std::bad_alloc&) {
        fprintf(stderr,"out of memory\n");
        return;
    } catch(uv_exception &e) {
        fprintf(stderr,"error: %s\n",uv_strerror(e.code));
        return;
    }

    ASSERT_UV_RESULT(uv_accept(server_,reinterpret_cast<uv_stream_t*>(&cc->client)));

    uv_read_start(
        reinterpret_cast<uv_stream_t*>(&cc->client),
        [](uv_handle_t *h,size_t suggest_s,uv_buf_t *buf) {
            *buf = uv_buf_init(new char[suggest_s],static_cast<unsigned int>(suggest_s));
        },
        [](uv_stream_t *s,ssize_t read,const uv_buf_t *buf) {
            auto cc = reinterpret_cast<client_connection*>(s->data);

            if(read < 0) {
                cc->close();
            } else if(read > 0) {
                try {
                    auto start = buf->base;
                    auto end = start + read;
                    auto zero = std::find(start,end,0);

                    for(;;) {
                        try {
                            while(zero != end) {
                                if(!parse_error) {
                                    cc->command_buffer.feed(start,zero-start);
                                    cc->command_buffer.finish();
                                    cc->register_task();
                                }
                                cc->command_buffer.reset();
                                parse_error = false;

                                start = zero + 1;
                                zero = std::find(start,end,0);
                            }

                            if(!parse_error) cc->command_buffer.feed(start,end);
                            break;
                        } catch(almost_json_parser::syntax_error&) {
                            parse_error = true;
                            queue_response(
                                stream,
                                copy_string(R"({"type":"error","errtype":"request","message":"invalid JSON"})"));
                        }
                    }
                } catch(std::bad_alloc&) {
                    fprintf(stderr,"insufficient memory for command");
                    cc->close();
                }
            }

            delete[] buf->base;
        });
}

void close_everything() {
    uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
    client_connection::close_all();
    uv_close(reinterpret_cast<uv_handle_t*>(&server),nullptr);
    uv_rwlock_destroy(&module_cache_lock);

    /* With everything closed, the loop should stop on its own. When debugging,
       we force the loop to end and print anything that didn't get closed. */
#ifndef NDEBUG
    uv_stop(main_loop);
#endif
}

void signal_handler(uv_signal_t*,int) {
    fprintf(stderr,"shutting down\n");
    close_everything();
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

int main(int argc, char* argv[]) {
    if(argc != 2) {
        fprintf(stderr,"exactly one argument is required\n");
        return 1;
    }

    v8::V8::InitializeICUDefaultLocation(argv[0]);
    v8::V8::InitializeExternalStartupData(argv[0]);
    platform = v8::platform::CreateDefaultPlatform();
    v8::V8::InitializePlatform(platform);
    v8::V8::Initialize();

    int r = 0;

    auto snapshot = create_snapshot();
    if(!snapshot.data) {
        fprintf(stderr,"failed to create start-up snapshot\n");
        goto end;
    }

    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    create_params.snapshot_blob = &snapshot;
    create_params.external_references = external_references;

    main_loop = uv_default_loop();

    if((r = uv_rwlock_init(&module_cache_lock))) {
        fprintf(stderr,"failed to initialize mutex: %s\n",uv_err_name(r));
        r = 2;
        goto loop_end;
    }

    if((r = uv_signal_init(main_loop,&sig_request))) {
        fprintf(stderr,"failed to initialize signal handler: %s\n",uv_err_name(r));
        uv_rwlock_destroy(&module_cache_lock);
        r = 2;
        goto loop_end;
    }

    if((r = uv_signal_start(&sig_request,&signal_handler,SIGINT))) {
        fprintf(stderr,"failed to initialize signal handler: %s\n",uv_err_name(r));
        uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
        uv_rwlock_destroy(&module_cache_lock);
        r = 2;
        goto loop_end;
    }

    if((r = uv_pipe_init(main_loop,&server,0))) {
        fprintf(stderr,"filed to initialize socket/pipe: %s\n",uv_err_name(r));
        uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
        uv_rwlock_destroy(&module_cache_lock);
        r = 2;
        goto loop_end;
    }

    if((r = uv_pipe_bind(&server,argv[1]))) {
        fprintf(stderr,"bind error: %s\n",uv_err_name(r));
        close_everything();
        r = 1;
        goto loop_end;
    }
    if((r = uv_listen(reinterpret_cast<uv_stream_t*>(&server),128,&on_new_connection))) {
        fprintf(stderr,"listen error: %s\n",uv_err_name(r));
        close_everything();
        r = 2;
        goto loop_end;
    }

    /* this is to let a parent process know that a socket/pipe is ready */
    printf("ready\n");
    fflush(stdout);

loop_end:
    /* even if we're shutting down, this still needs to be called to release
       resources */
    uv_run(main_loop,UV_RUN_DEFAULT);

#ifndef NDEBUG
    {
        int r2 = uv_loop_close(main_loop);
        if(r2) uv_print_all_handles(main_loop,stderr);
    }
#else
    uv_loop_close(main_loop);
#endif

end:
    module_cache.clear();
    delete[] snapshot.data;
    v8::V8::Dispose();
    v8::V8::ShutdownPlatform();
    delete create_params.array_buffer_allocator;
    delete platform;

    return r;
}

