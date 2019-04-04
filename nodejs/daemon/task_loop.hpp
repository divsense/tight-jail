#ifndef task_loop_hpp
#define task_loop_hpp

#include <memory>
#include <queue>
#include <vector>
#include <map>
#include <atomic>
#include <v8.h>
#include <libplatform/libplatform.h>

#include "uvwrap.hpp"

class task_loop : public v8::TaskRunner {
    typedef std::pair<double,std::unique_ptr<v8::Task>> delayed_task_t;
    struct delayed_compare {
        bool operator()(delayed_task_t &a,delayed_task_t &b) {
            return a.first < b.first;
        }
    };
    enum status_t {NORMAL=0,CLOSING,CLOSED};

    v8::Isolate *isolate;
    std::queue<std::unique_ptr<v8::Task>> task_queue;
    std::priority_queue<delayed_task_t,std::vector<delayed_task_t>,delayed_compare> delayed_task_queue;
    uvwrap::loop_t loop;
    uvwrap::mutex_t task_lock;
    uvwrap::async_t dispatcher;
    uvwrap::async_t micro_dispatcher;
    uvwrap::timer_t delay_timer;
    uv_thread_t thread;
    std::atomic<int> status;

    void set_timer(double timeout);

public:
    task_loop(v8::Isolate *isolate);
    ~task_loop();

    void PostTask(std::unique_ptr<v8::Task> task);
    void PostDelayedTask(std::unique_ptr<v8::Task> task,double delay_in_seconds);
    void PostIdleTask(std::unique_ptr<v8::IdleTask> task);
    bool IdleTasksEnabled();

    void close();

    void signal_microtasks() {
        micro_dispatcher.send(std::nothrow);
    }
};


class v8_platform : public v8::Platform {
    uvwrap::mutex_t runner_lock;

    /* We need to use our own task runners but everything else is deferred to
    the default platform */
    std::unique_ptr<v8::Platform> default_plat;

    std::map<v8::Isolate*,std::shared_ptr<task_loop>> runner_map;

public:
    std::shared_ptr<task_loop> get_task_loop(v8::Isolate* isolate) {
        uvwrap::mutex_scope_lock lock{runner_lock};

        auto itr = runner_map.find(isolate);
        if(itr == runner_map.end()) {
            auto runner = std::make_shared<task_loop>(isolate);
            runner_map.insert({isolate,runner});
            return runner;
        }
        return itr->second;
    }

    void remove_task_loop(v8::Isolate* isolate) {
        uvwrap::mutex_scope_lock lock{runner_lock};
        runner_map.erase(isolate);
    }

    size_t remaining_runners() {
        uvwrap::mutex_scope_lock lock{runner_lock};
        return runner_map.size();
    }

    v8::PageAllocator* GetPageAllocator() {
        return default_plat->GetPageAllocator();
    }
    void OnCriticalMemoryPressure() {
        default_plat->OnCriticalMemoryPressure();
    }
    int NumberOfWorkerThreads() {
        return default_plat->NumberOfWorkerThreads();
    }
    std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(v8::Isolate* isolate) {
        return get_task_loop(isolate);
    }
    void CallOnWorkerThread(std::unique_ptr<v8::Task> task) {
        default_plat->CallOnWorkerThread(std::move(task));
    }
    void CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task,double delay_in_seconds) {
        default_plat->CallDelayedOnWorkerThread(std::move(task),delay_in_seconds);
    }
    void CallOnForegroundThread(v8::Isolate* isolate,v8::Task* task) {
        GetForegroundTaskRunner(isolate)->PostTask(std::unique_ptr<v8::Task>{task});
    }
    void CallDelayedOnForegroundThread(v8::Isolate* isolate,v8::Task* task,double delay_in_seconds) {
        GetForegroundTaskRunner(isolate)->PostDelayedTask(std::unique_ptr<v8::Task>{task},delay_in_seconds);
    }
    double MonotonicallyIncreasingTime();
    double CurrentClockTimeMillis() {
        return default_plat->CurrentClockTimeMillis();
    }
    v8::TracingController* GetTracingController() {
        return default_plat->GetTracingController();
    }

    v8_platform() : default_plat{v8::platform::CreateDefaultPlatform()} {}
};

#endif

