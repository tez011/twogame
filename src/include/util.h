#pragma once
#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <vector>
#include <volk.h>
#include "vk_mem_alloc.h"

namespace twogame {
class Renderer;
}

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

std::string resolve_path(std::string_view current, std::string_view relative);

template <typename T>
bool parse(const std::string_view&, T&);

}

namespace twogame::vk {

size_t format_width(VkFormat);

typedef struct {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo details;
} buffer;

class BufferPool {
    const Renderer& m_renderer;
    std::vector<buffer> m_buffers;
    std::vector<bool> m_bits;
    std::vector<bool>::iterator m_bits_it;
    VkDeviceSize m_unit_size, m_count;
    VkBufferUsageFlags m_usage;

    void extend();

public:
    using index_t = uint32_t;
    BufferPool(const Renderer&, VkBufferUsageFlags usage, size_t unit_size, size_t count = 0x4000);
    ~BufferPool();

    index_t allocate();
    void free(index_t);

    VkDeviceSize unit_size() const { return m_unit_size; }
    void buffer_handle(index_t index, VkDescriptorBufferInfo& out) const;
    void* buffer_memory(index_t index, size_t extra_offset = 0) const;
};

class DescriptorPool {
private:
    VkDevice m_device;
    uint32_t m_max_sets;
    std::vector<VkDescriptorPoolSize> m_sizes;

    VkDescriptorSetLayout m_set_layout;
    std::deque<VkDescriptorPool> m_pools, m_pools_full;
    std::deque<VkDescriptorSet> m_free_list;

    void extend();

public:
    DescriptorPool(const Renderer&, const VkDescriptorSetLayoutCreateInfo& layout_info, uint32_t max_sets = 1024);
    ~DescriptorPool();

    inline const VkDescriptorSetLayout& layout() const { return m_set_layout; }

    int allocate(VkDescriptorSet* out, size_t count = 1);
    void free(VkDescriptorSet* sets, size_t count = 1);
    void reset();
};

}
