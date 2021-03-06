/* Exception-safe wrappers.
 *
 * Note that because libuv handles are freed in the event loop, instead of
 * immediately, the memory holding objects that depend on "loop_t" must remain
 * valid until the next call to "run", or until the destructor of "loop_t" is
 * called (which also calls "run").
 */

#ifndef uvwrap_hpp
#define uvwrap_hpp

#include <uv.h>
#include <exception>
#include <assert.h>
#include <new> // for nothrow_t


#ifdef NDEBUG
#  define ASSERT_UV_RESULT(X) (X)
#else
#  define ASSERT_UV_RESULT(X) assert((X) == 0)
#endif

namespace uvwrap {
    struct uv_error : std::exception {
        int code;
        uv_error(int code) : code{code} {}
        const char *what() const noexcept override { return uv_strerror(code); }
    };

    inline void check_err(int r) {
        if(r) throw uv_error{r};
    }

    struct loop_t {
        uv_loop_t data;

        loop_t() {
            check_err(uv_loop_init(&data));
        }

        ~loop_t() {
            uv_run(&data,UV_RUN_DEFAULT); // needed to free any handles
            ASSERT_UV_RESULT(uv_loop_close(&data));
        }

        int run(uv_run_mode mode) {
            return uv_run(&data,mode);
        }

        void stop() noexcept {
            uv_stop(&data);
        }
    };

    struct async_t {
        uv_async_t data;

        async_t(loop_t &loop,uv_async_cb cb) {
            check_err(uv_async_init(&loop.data,&data,cb));
        }

        async_t(loop_t &loop,uv_async_cb cb,void *user) {
            check_err(uv_async_init(&loop.data,&data,cb));
            data.data = user;
        }

        ~async_t() {
            uv_close(reinterpret_cast<uv_handle_t*>(&data),nullptr);
        }

        void send() {
            check_err(uv_async_send(&data));
        }

        void send(std::nothrow_t) {
            ASSERT_UV_RESULT(uv_async_send(&data));
        }
    };

    struct mutex_t {
        uv_mutex_t data;

        mutex_t() {
            check_err(uv_mutex_init(&data));
        }

        ~mutex_t() { uv_mutex_destroy(&data); }
    };

    class mutex_scope_lock {
        uv_mutex_t &m;

    public:
        mutex_scope_lock(mutex_t &mut) : m{mut.data} {
            uv_mutex_lock(&m);
        }

        ~mutex_scope_lock() {
            uv_mutex_unlock(&m);
        }
    };

    struct rwlock_t {
        uv_rwlock_t data;

        rwlock_t() {
            check_err(uv_rwlock_init(&data));
        }

        ~rwlock_t() { uv_rwlock_destroy(&data); }
    };

    class read_scope_lock {
        uv_rwlock_t &rw;

    public:
        read_scope_lock(rwlock_t &rwlock) : rw{rwlock.data} {
            uv_rwlock_rdlock(&rw);
        }

        ~read_scope_lock() {
            uv_rwlock_rdunlock(&rw);
        }
    };

    class write_scope_lock {
        uv_rwlock_t &rw;

    public:
        write_scope_lock(rwlock_t &rwlock) : rw{rwlock.data} {
            uv_rwlock_wrlock(&rw);
        }

        ~write_scope_lock() {
            uv_rwlock_wrunlock(&rw);
        }
    };

    struct signal_t {
        uv_signal_t data;

        signal_t(loop_t &loop) {
            check_err(uv_signal_init(&loop.data,&data));
        }

        ~signal_t() {
            uv_close(reinterpret_cast<uv_handle_t*>(&data),nullptr);
        }

        void start(uv_signal_cb cb,int signum) {
            check_err(uv_signal_start(&data,cb,signum));
        }

        void stop() {
            check_err(uv_signal_stop(&data));
        }

        void stop(std::nothrow_t) noexcept {
            ASSERT_UV_RESULT(uv_signal_stop(&data));
        }
    };

    struct pipe_t {
        uv_pipe_t data;

        uv_handle_t *as_uv_handle() {
            return reinterpret_cast<uv_handle_t*>(&data);
        }

        uv_stream_t *as_uv_stream() {
            return reinterpret_cast<uv_stream_t*>(&data);
        }

        pipe_t(loop_t &loop,bool ipc=false) {
            check_err(uv_pipe_init(&loop.data,&data,ipc ? 1 : 0));
        }

        pipe_t(loop_t &loop,bool ipc,void *user_ptr) {
            check_err(uv_pipe_init(&loop.data,&data,ipc ? 1 : 0));
            data.data = user_ptr;
        }

        pipe_t(loop_t &loop,void *user_ptr) {
            check_err(uv_pipe_init(&loop.data,&data,0));
            data.data = user_ptr;
        }


        void bind(const char *name) {
            check_err(uv_pipe_bind(&data,name));
        }

        void open(uv_file file) {
            check_err(uv_pipe_open(&data,file));
        }

        void listen(int backlog,uv_connection_cb cb) {
            check_err(uv_listen(reinterpret_cast<uv_stream_t*>(&data),backlog,cb));
        }

        void accept(pipe_t &client) {
            check_err(uv_accept(as_uv_stream(),client.as_uv_stream()));
        }

        void read_start(uv_read_cb read) {
            check_err(uv_read_start(
                as_uv_stream(),
                [](uv_handle_t *h,size_t suggest_s,uv_buf_t *buf) {
                    *buf = uv_buf_init(new char[suggest_s],static_cast<unsigned int>(suggest_s));
                },
                read));
        }

        void read_stop() {
            check_err(uv_read_stop(as_uv_stream()));
        }

        void read_stop(std::nothrow_t) noexcept {
            ASSERT_UV_RESULT(uv_read_stop(as_uv_stream()));
        }

    protected:
        ~pipe_t() {}
    };

    struct pipe_t_auto : pipe_t {
        using pipe_t::pipe_t;

        ~pipe_t_auto() {
            uv_close(as_uv_handle(),nullptr);
        }
    };

    struct pipe_t_manual : pipe_t {
        using pipe_t::pipe_t;

        void close(uv_close_cb callback=nullptr) {
            uv_close(as_uv_handle(),callback);
        }
    };

    struct timer_t {
        uv_timer_t data;

        uv_handle_t *as_uv_handle() {
            return reinterpret_cast<uv_handle_t*>(&data);
        }

        timer_t(loop_t &loop) {
            check_err(uv_timer_init(&loop.data,&data));
        }

        timer_t(loop_t &loop,void *user_ptr) {
            check_err(uv_timer_init(&loop.data,&data));
            data.data = user_ptr;
        }

        ~timer_t() {
            uv_close(as_uv_handle(),nullptr);
        }

        void start(uv_timer_cb callback,uint64_t timeout,uint64_t repeat=0) {
            check_err(uv_timer_start(&data,callback,timeout,repeat));
        }
    };

    struct cond_t {
        uv_cond_t data;

        cond_t() {
            check_err(uv_cond_init(&data));
        }

        ~cond_t() {
            uv_cond_destroy(&data);
        }

        void signal() {
            uv_cond_signal(&data);
        }

        void broadcast() {
            uv_cond_broadcast(&data);
        }

        void wait(mutex_t &m) {
            uv_cond_wait(&data,&m.data);
        }
    };
}

#endif

