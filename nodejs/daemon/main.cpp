#include <libplatform/libplatform.h>
#include <v8.h>
#include <uv.h>
#include <memory>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>
#include <string.h>
#include <ctype.h>
#include <assert.h>


uv_loop_t *loop;
uv_pipe_t server;
uv_signal_t sig_request;

v8::Isolate::CreateParams create_params;
v8::Platform *platform;

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
        if(prev) prev->next = node;
        node->next = this;
        prev = node;
    }

    void insert_after(list_node *node) {
        assert(!(node->next || node->prev));
        if(next) next->prev = node;
        node->prev = this;
        next = node;
    }

protected:
    ~list_node() {
        assert(!(next || prev));
    }
};

struct request_task;

/* The lifetime management for this is a little unusual. When "close()" is
 * called, it schedules a callback to delete itself, but if there are still any
 * request instances that belong to this, by the time the callback is called,
 * it won't delete itself and instead the last request to be destroyed will
 * delete this. */
struct client_connection : private list_node {
    typedef std::map<unsigned long,v8::UniquePersistent<v8::Context>> context_map;

    static client_connection *conn_head;

    uv_pipe_t client;
    v8::Isolate* isolate;
    std::vector<char> command_buffer;
    v8::UniquePersistent<v8::Context> request_context;
    request_task *requests_head;
    context_map contexts;
    unsigned long next_id;

    /* only one thread per isolate can be used */
    volatile bool running;

    /* when true, the last request to reference this connection will delete this
     */
    bool closed;

    enum {
        STR_TYPE=0,
        STR_FUNC,
        STR_ARGS,
        STR_CONTEXT,
        STR_URI,
        STR_CODE,
        STR_CREATE_CONTEXT,
        STR_DESTROY_CONTEXT,
        STR_LOAD_SCRIPT,
        STR_EVAL,
        STR_EXEC,
        STR_CALL,
        STR_GETSTATS,
        COUNT_STR
    };

    v8::UniquePersistent<v8::String> str[COUNT_STR];
    static const char *str_raw[COUNT_STR];

    client_connection() : requests_head(nullptr), next_id(1), running(false), closed(false) {
        uv_pipe_init(loop,&client,0);
        client.data = this;

        isolate = v8::Isolate::New(create_params);

        if(conn_head) conn_head->insert_before(this);
        conn_head = this;
    }

    ~client_connection() {
        assert(!requests_head);
        remove();
        if(this == conn_head) conn_head = nullptr;

        /* all the JS references need to be freed before calling
           isolate->Dispose */
        for(auto &item : str) item.Reset();
        request_context.Reset();
        contexts.clear();

        isolate->Dispose();
    }

    v8::Local<v8::String> get_str(unsigned int i);

    void close();

    bool is_closing() const {
        return uv_is_closing(reinterpret_cast<const uv_handle_t*>(&client));
    }

    void register_task(request_task *task);

    static void close_all() {
        for(auto itr = conn_head; itr; itr = static_cast<client_connection*>(itr->next)) itr->close();
    }
};

client_connection *client_connection::conn_head = nullptr;

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
    "loadscript",
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
    friend class client_connection;

    /* libuv already queues tasks for a set number of worker threads, but tasks
       that belong to the same v8::Isolate instance must not be run
       concurrently, thus only one task per Isolate is given to libuv at a time.

       It is expected that most of the time, a client will wait for a task to
       complete before sending another, and thus this container won't have more
       than a few tasks at any given time. */
    static std::vector<request_task*> tasks;

    static uv_async_t work_dispatcher;
    static uv_mutex_t tasks_mutex;

    static void enqueue(client_connection *client,const char *msg,size_t msg_size) {
        {
            mutex_scope_lock lock{tasks_mutex};
            tasks.push_back(new request_task(client,msg,msg_size));
        }
        run_dispatcher();
    }

    static void run_dispatcher() {
        int r = uv_async_send(&work_dispatcher);
        if(r) {
            fprintf(stderr,"failed to signal work dispatcher, %s\n",uv_strerror(r));
        }
    }

    static void dispatch_work(uv_async_t*);

    static void purge_nulls() {
        auto new_end = std::remove_if(tasks.begin(),tasks.end(),[](request_task *t) { return !t; });
        tasks.resize(new_end - tasks.begin());
    }

    static void clear_pending(client_connection *cc=nullptr) {
        for(auto &t : tasks) {
            if(!cc || cc == t->client) {
                delete t;
                cc = nullptr;
            }
        }
        purge_nulls();
    }

    client_connection *client;
    std::unique_ptr<char[]> message;
    bool cancelled;

    request_task(client_connection *client,const char *msg,size_t msg_size) :
            client(client),
            message(new char[msg_size + 1]),
            cancelled(false)
    {
        //thread_req.data = this;
        memcpy(message.get(),msg,msg_size);
        message[msg_size] = 0;
        client->register_task(this);
    }

    ~request_task() {
        remove();
        if(client->requests_head == this) {
            if(client->closed) delete client;
            else client->requests_head = nullptr;
        }
    }

    void Run() override;

    void cancel() {
        cancelled = true;
    }
};

void client_connection::register_task(request_task *task) {
    if(requests_head) requests_head->insert_before(task);
    requests_head = task;
}

void client_connection::close() {
    if(!is_closing()) {
        isolate->TerminateExecution();
        request_task::clear_pending(this);
        for(auto itr = requests_head; itr; itr = static_cast<request_task*>(itr->next)) itr->cancel();
        uv_close(
            reinterpret_cast<uv_handle_t*>(&client),
            [](uv_handle_t *h) {
                auto self = reinterpret_cast<client_connection*>(h->data);
                if(self->requests_head) self->closed = true;
                else delete self;
            });
    }
}

std::vector<request_task*> request_task::tasks;
uv_async_t request_task::work_dispatcher;
uv_mutex_t request_task::tasks_mutex;

void queue_response(uv_stream_t *stream,char *msg,ssize_t len=-1) {
    if(len < 0) len = strlen(msg);
    auto req = new write_request(msg);
    char zero[1] = {0};
    uv_buf_t buf[2] = {uv_buf_init(msg,static_cast<size_t>(len)),uv_buf_init(zero,1)};
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
        buffer.insert(buffer.end(),str->Utf8Length(isolate),0);
        str->WriteUtf8(isolate,buffer.data() + length);
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
            if(c <= 0x7f && isprint(c)) add_char(c);
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

    void add_part(long value) {
        build.add_integer(value);
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

    queue_response(stream,tmp.get_string());
}

std::pair<unsigned long,v8::Local<v8::Context>> get_context(client_connection *cc,v8::Local<v8::Object> msg) {
    v8::Local<v8::Value> js_id = check_r(msg->Get(cc->get_str(client_connection::STR_CONTEXT)));
    if(js_id->IsNullOrUndefined()) return std::make_pair(0,v8::Context::New(cc->isolate));

    int64_t id = check_r(js_id->ToInteger(cc->request_context.Get(cc->isolate)))->Value();
    if(id < 0) throw bad_context{};
    auto itr = cc->contexts.find(id);
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

void request_task::Run() {
    using namespace v8;

    if(cancelled) return;

    Locker locker{client->isolate};

    uv_stream_t *stream = reinterpret_cast<uv_stream_t*>(&client->client);

    Isolate::Scope isolate_scope{client->isolate};
    HandleScope handle_scope{client->isolate};

    if(client->request_context.IsEmpty()) {
        client->request_context.Reset(client->isolate,Context::New(client->isolate));
    }
    Local<Context> request_context = client->request_context.Get(client->isolate);
    Context::Scope context_scope(request_context);

    TryCatch trycatch(client->isolate);

    try {
        Local<Object> msg = check_r(check_r(JSON::Parse(
            request_context,
            check_r(String::NewFromUtf8(
                client->isolate,
                message.get(),
                NewStringType::kNormal))))->ToObject(request_context));

        Local<String> type = check_r(check_r(msg->Get(
            client->get_str(client_connection::STR_TYPE)))->ToString(request_context));


        /*if(type->StrictEquals(client->get_str(client_connection::STR_LOAD_SCRIPT))) {
            Local<String> uri = check_r(check_r(msg->Get(
                client->get_str(client_connection::STR_URI)))->ToString(request_context));
            auto context = get_context(client,msg);
            Context::Scope context_scope(context.second);
        }
        else */if(type->StrictEquals(client->get_str(client_connection::STR_EVAL))) {
            Local<String> code = check_r(check_r(
                msg->Get(client->get_str(client_connection::STR_CODE)))->ToString(request_context));
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
            } else if(!client->is_closing()) {
                queue_script_exception(stream,trycatch,client->isolate,context.second);
            }
        }
        else if(type->StrictEquals(client->get_str(client_connection::STR_EXEC))) {
            Local<String> code = check_r(check_r(
                msg->Get(client->get_str(client_connection::STR_CODE)))->ToString(request_context));
            auto context = get_context(client,msg);
            Context::Scope context_scope(context.second);

            Local<Script> compiled;
            if(Script::Compile(context.second,code).ToLocal(&compiled) && !compiled->Run(context.second).IsEmpty()) {
                response_builder b{client->isolate};
                b("type","\"success\"");
                if(context.first) b("context",context.first);
                b.send(stream);
            } else if(!client->is_closing()) {
                queue_script_exception(stream,trycatch,client->isolate,context.second);
            }
        }
        else if(type->StrictEquals(client->get_str(client_connection::STR_CALL))) {
            Local<String> fname = check_r(check_r(
                msg->Get(client->get_str(client_connection::STR_FUNC)))->ToString(request_context));
            Local<Value> tmp = check_r(
                msg->Get(client->get_str(client_connection::STR_ARGS)));

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
                    if(func->Call(context.second,context.second->Global(),args_v.size(),args_v.data()).ToLocal(&result) &&
                            JSON::Stringify(context.second,result).ToLocal(&json_result)) {
                        response_builder b{client->isolate};
                        b("type","\"result\"");
                        b("value",*json_result);
                        if(context.first) b("context",context.first);
                        b.send(stream);
                    } else if(!client->is_closing()) {
                        queue_script_exception(stream,trycatch,client->isolate,context.second);
                    }
                } else {
                    string_builder b;
                    b.add_string(R"({"type":"error","errtype":"request","message":")");
                    b.add_escaped(String::Value{client->isolate,fname});
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
        else if(type->StrictEquals(client->get_str(client_connection::STR_CREATE_CONTEXT))) {
            unsigned long id = client->next_id++;
            client->contexts.emplace(
                id,
                UniquePersistent<Context>{client->isolate,Context::New(client->isolate)});

            response_builder{client->isolate}
                ("type","\"success\"")
                ("context",id).send(stream);
        }
        else if(type->StrictEquals(client->get_str(client_connection::STR_DESTROY_CONTEXT))) {
            int64_t id = check_r(check_r(msg->Get(client->get_str(client_connection::STR_CONTEXT)))
                ->ToInteger(client->request_context.Get(client->isolate)))->Value();
            if(id < 0) throw bad_context{};
            auto itr = client->contexts.find(id);
            if(itr == client->contexts.end()) throw bad_context{};
            client->contexts.erase(itr);

            response_builder{client->isolate}
                ("type","\"success\"")
                ("context",id).send(stream);
        }
        else if(type->StrictEquals(client->get_str(client_connection::STR_GETSTATS))) {
            response_builder{client->isolate}
                ("type","\"result\"")
                ("value","{}").send(stream);
        }
        else {
            if(!client->is_closing())
                queue_response(
                    stream,
                    copy_string(R"({"type":"error","errtype":"request","message":"unknown request type"})"));
        }
    } catch(std::bad_alloc&) {
        if(!client->is_closing())
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"memory","message":"insufficient memory"})"));
    } catch(bad_request&) {
        if(!client->is_closing())
            queue_js_exception(stream,trycatch,client->isolate,request_context);
    } catch(bad_context&) {
        if(!client->is_closing())
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"request","message":"no such context"})"));
    } catch(...) {
        if(!client->is_closing())
            queue_response(
                stream,
                copy_string(R"({"type":"error","errtype":"internal","message":"unexpected error"})"));
    }

    client->running = false;
    request_task::run_dispatcher();
}

void on_new_connection(uv_stream_t *server,int status) noexcept {
    if(status < 0) {
        fprintf(stderr,"connection error %s\n",uv_strerror(status));
        return;
    }

    auto cc = new(std::nothrow) client_connection;
    if(!cc) {
        fprintf(stderr,"out of memory\n");
        return;
    }

#ifndef NDEBUG
    int r =
#endif
    uv_accept(server,reinterpret_cast<uv_stream_t*>(&cc->client));
    assert(r == 0);

    uv_read_start(
        reinterpret_cast<uv_stream_t*>(&cc->client),
        [](uv_handle_t *h,size_t suggest_s,uv_buf_t *buf) {
            buf->base = new char[suggest_s];
            buf->len = suggest_s;
        },
        [](uv_stream_t *s,ssize_t read,const uv_buf_t *buf) {
            auto cc = reinterpret_cast<client_connection*>(s->data);

            if(read < 0) {
                cc->close();
            } else if(read > 0) {
                try {
                    auto &cbuf = cc->command_buffer;
                    size_t start = cbuf.size();
                    cbuf.insert(cbuf.end(),buf->base,buf->base + read);
                    auto zero = std::find(cbuf.begin() + start,cbuf.end(),0);

                    while(zero != cbuf.end()) {
                        request_task::enqueue(cc,cbuf.data(),zero - cbuf.begin());

                        cbuf.erase(cbuf.begin(),zero + 1);

                        zero = std::find(cbuf.begin(),cbuf.end(),0);
                    }
                } catch(std::bad_alloc&) {
                    fprintf(stderr,"insufficient memory for command");
                    cc->close();
                }
            }

            delete[] buf->base;
        });
}

void request_task::dispatch_work(uv_async_t*) {
    mutex_scope_lock lock{tasks_mutex};

    // each vector element that is dispatched is set to nullptr
    for(size_t i=0; i<tasks.size(); ++i) {
        if(!tasks[i]->client->running) {
            tasks[i]->client->running = true;

            try {
                platform->CallOnWorkerThread(std::unique_ptr<request_task>(tasks[i]));
            } catch(...) {
                tasks[i]->client->running = false;
                queue_response(
                    reinterpret_cast<uv_stream_t*>(&tasks[i]->client->client),
                    copy_string(R"({"type":"error","errtype":"internal","message":"failed to queue work"})"));
            }
            tasks[i] = nullptr;
        }
    }

    purge_nulls();
}

void close_everything() {
    uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
    client_connection::close_all();
    uv_close(reinterpret_cast<uv_handle_t*>(&server),nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&request_task::work_dispatcher),nullptr);
}

void signal_handler(uv_signal_t *req,int signum) {
    fprintf(stderr,"shutting down\n");
    close_everything();
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

    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    int r = 0;
    if((r = uv_mutex_init(&request_task::tasks_mutex))) {
        fprintf(stderr,"failed to create mutex: %s\n",uv_err_name(r));
        r = 2;
        goto end;
    }

    loop = uv_default_loop();

    if((r = uv_async_init(loop,&request_task::work_dispatcher,&request_task::dispatch_work))) {
          fprintf(stderr,"failed to initialize work dispatcher: %s\n",uv_err_name(r));
         r = 2;
        goto loop_end;
    }

    if((r = uv_signal_init(loop,&sig_request))) {
        fprintf(stderr,"failed to initialize signal handler: %s\n",uv_err_name(r));
        uv_close(reinterpret_cast<uv_handle_t*>(&request_task::work_dispatcher),nullptr);
        r = 2;
        goto loop_end;
    }

    if((r = uv_signal_start(&sig_request,&signal_handler,SIGINT))) {
        fprintf(stderr,"failed to initialize signal handler: %s\n",uv_err_name(r));
        uv_close(reinterpret_cast<uv_handle_t*>(&request_task::work_dispatcher),nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
        r = 2;
        goto loop_end;
    }

    if((r = uv_pipe_init(loop,&server,0))) {
        fprintf(stderr,"filed to initialize socket/pipe: %s\n",uv_err_name(r));
        uv_close(reinterpret_cast<uv_handle_t*>(&request_task::work_dispatcher),nullptr);
        uv_close(reinterpret_cast<uv_handle_t*>(&sig_request),nullptr);
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
    uv_run(loop,UV_RUN_DEFAULT);

    uv_mutex_destroy(&request_task::tasks_mutex);

#ifndef NDEBUG
    {
        int r2 = uv_loop_close(loop);
        if(r2) uv_print_all_handles(loop,stderr);
    }
#else
    uv_loop_close(loop);
#endif

end:
    v8::V8::Dispose();
    v8::V8::ShutdownPlatform();
    delete create_params.array_buffer_allocator;
    delete platform;

    for(auto task : request_task::tasks) delete task;

    return r;
}

