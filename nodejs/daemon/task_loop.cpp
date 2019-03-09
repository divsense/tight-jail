#include <assert.h>
#include <chrono>

#include "task_loop.hpp"

v8_platform *platform = nullptr;

namespace {
    double steady_seconds() {
        std::chrono::duration<double> now = std::chrono::steady_clock::now().time_since_epoch();
        return now.count();
    }
}

void task_loop::set_timer(double timeout) {
    delay_timer.start(
        [](uv_timer_t *handle) {
            reinterpret_cast<task_loop*>(handle->data)->dispatcher.send(std::nothrow);
        },
        static_cast<uint64_t>(std::max(timeout,0.0) * 1000),
        0);
}

task_loop::task_loop(v8::Isolate *isolate) :
    isolate{isolate},
    dispatcher{
        loop,
        [](uv_async_t *handle) {
            auto tloop = reinterpret_cast<task_loop*>(handle->data);


            for(;;) {
                auto s = tloop->status.load();
                if(s) {
                    if(s == CLOSING) tloop->loop.stop();
                    return;
                }

                {
                    std::unique_ptr<v8::Task> task;
                    {
                        uvwrap::mutex_scope_lock lock{tloop->task_lock};
                        if(tloop->task_queue.empty()) break;
                        std::swap(task,tloop->task_queue.front());
                        tloop->task_queue.pop();
                    }

                    task->Run();
                }
                v8::Locker lock{tloop->isolate};
                tloop->isolate->RunMicrotasks();
            }

            for(;;) {
                auto s = tloop->status.load();
                if(s) {
                    if(s == CLOSING) tloop->loop.stop();
                    return;
                }

                {
                    std::unique_ptr<v8::Task> task;
                    double now = steady_seconds();
                    {
                        uvwrap::mutex_scope_lock lock{tloop->task_lock};
                        if(tloop->delayed_task_queue.empty() ||
                            tloop->delayed_task_queue.top().first > now) break;

                        /* std::priority_queue lacks a way to do this without a
                        cast */
                        std::swap(task,const_cast<std::unique_ptr<v8::Task>&>(tloop->delayed_task_queue.top().second));

                        tloop->delayed_task_queue.pop();
                    }

                    task->Run();
                }
                v8::Locker lock{tloop->isolate};
                tloop->isolate->RunMicrotasks();
            }

            uvwrap::mutex_scope_lock lock{tloop->task_lock};
            if(!tloop->delayed_task_queue.empty()) {
                tloop->set_timer(tloop->delayed_task_queue.top().first - steady_seconds());
            }
        },
        this},
    delay_timer{loop,this},
    status{NORMAL}
{
    int r;

    if((r = uv_thread_create(
        &thread,
        [](void *arg) {
            auto tloop = reinterpret_cast<task_loop*>(arg);
            tloop->loop.run(UV_RUN_DEFAULT);
        },
        this)))
    {
        throw uvwrap::uv_error{r};
    }
}

task_loop::~task_loop() {
    close();
}

void task_loop::PostTask(std::unique_ptr<v8::Task> task) {
    {
        uvwrap::mutex_scope_lock lock{task_lock};

        task_queue.push(std::move(task));
    }
    dispatcher.send(std::nothrow);
}

void task_loop::PostDelayedTask(std::unique_ptr<v8::Task> task,double delay_in_seconds) {
    bool do_set;
    {
        uvwrap::mutex_scope_lock lock{task_lock};

        double when = delay_in_seconds + steady_seconds();
        do_set = when < delayed_task_queue.top().first;
        delayed_task_queue.push({when,std::move(task)});
    }

    if(do_set) set_timer(delay_in_seconds);
    dispatcher.send(std::nothrow);
}

void task_loop::PostIdleTask(std::unique_ptr<v8::IdleTask> task) {
    assert(false);
}

bool task_loop::IdleTasksEnabled() { return false; }


double v8_platform::MonotonicallyIncreasingTime() {
    return steady_seconds();
}

void task_loop::close() {
    if(status.load() == NORMAL) {
        status.store(CLOSING);
        ASSERT_UV_RESULT(uv_async_send(&dispatcher.data));
        ASSERT_UV_RESULT(uv_thread_join(&thread));
        status.store(CLOSED);
    }
}

