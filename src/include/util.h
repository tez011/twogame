#pragma once
#include <functional>
#include <future>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

namespace twogame::util {

class ThreadPool {
    class Thread {
        std::thread m_thread;
        int m_thread_id;

        std::queue<std::function<void()>> m_tasks;
        std::mutex m_mtx_tasks;
        std::condition_variable m_cv_tasks;
        bool m_destroying;

    public:
        Thread(int thread_id);
        ~Thread();

        void wait_idle();
        void work();

        template <typename F, typename... P, typename R = std::invoke_result_t<F, P...>>
        std::future<R> submit(F&& fn, P&&... params)
        {
            std::promise<R> p;
            std::future<R> task_future = p.get_future();
            auto ptask = [task_function = std::bind(std::forward<F>(fn), std::forward<P>(params)...), task_promise = std::move(p)]() {
                try {
                    if constexpr (std::is_void_v<R>) {
                        std::invoke(task_function);
                        task_promise.set_value();
                    } else {
                        task_promise.set_value(std::invoke(task_function));
                    }
                } catch (...) {
                    task_promise.set_exception(std::current_exception());
                }
            };

            {
                std::scoped_lock lk(m_mtx_tasks);
                m_tasks.push(std::bind(ptask));
                m_cv_tasks.notify_one();
            }
            return task_future;
        }
    };

    std::vector<std::unique_ptr<Thread>> m_threads;

public:
    ThreadPool(size_t thread_count = 0);
    static int current_thread_id();
    inline size_t thread_count() const { return m_threads.size(); }
    inline Thread* thread(int n) { return m_threads[n - 1].get(); }
    void wait_idle();
};
}
