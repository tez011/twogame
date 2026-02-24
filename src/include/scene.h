#pragma once
#include <atomic>
#include <list>
#include <queue>
#include <span>
#include <stack>
#include <thread>
#include <unordered_map>
#include "display.h"
#include "mpmc.h"

namespace twogame {

class SceneHost final {
public:
    constexpr static VkDeviceSize STAGING_BUFFER_SIZE = 1 << 29;

private:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;
    struct QData {
        IScene* scene;
        VkCommandBuffer commands;
        uint64_t ticket;
    };

    std::atomic<IScene*> m_active_scene;
    std::atomic_uint32_t m_frame_number = 0;
    MPMCQ<QData, 8> m_builder_queue, m_render_queue, m_return_queue;
    MPMCQ<SDL_Event, 64> m_event_queue;

    // Owned by scene thread
    using frame_number_t = uint32_t;
    IScene* m_requested_scene;
    std::thread m_scene_host;
    std::unordered_map<IScene*, uint64_t> m_scenes;
    std::queue<std::pair<IScene*, frame_number_t>> m_purge_queue;
    std::stack<VkCommandBuffer> m_spare_commands;
    std::atomic_uint64_t m_max_ticket;
    bool m_active;

    // Owned by render thread
    IRenderer* r_renderer;
    VkCommandPool m_command_pool;
    VkQueue m_transfer_queue;
    VkSemaphore m_timeline;

    // Owned by worker threads
    struct StagingBuffer {
        VkBuffer buffer;
        VmaAllocation mem;
        unsigned char* ptr;
    };
    constexpr static int BUILDER_THREAD_COUNT = 2;
    std::array<std::thread, BUILDER_THREAD_COUNT> m_builders;
    std::array<StagingBuffer, BUILDER_THREAD_COUNT> m_staging_buffers;

    void scene_loop();
    void builder_loop(int thread_id);

public:
    SceneHost(IRenderer* renderer, IScene* initial);
    ~SceneHost();

    /**
     * Enqueue the scene for preparation.
     * @warning only safe to call from the scene thread.
     * @return false if the queue is full and the caller should try again.
     */
    bool prepare(IScene* scene);

    /**
     * Set the next scene. When this next scene is ready, the host will switch to it.
     * @warning only safe to call from the scene thread.
     */
    void set_next_scene(IScene* scene);

    void wait_frame(uint32_t frame_number);
    void push_event(SDL_Event*);
    void tick();

    void execute_draws(VkCommandBuffer container, uint32_t frame_number, int subpass);
};

class IScene {
    friend class SceneHost;

protected:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;

    VkDevice r_device;
    VmaAllocator r_allocator;
    IScene(DisplayHost* host)
        : r_device(host->device())
        , r_allocator(host->allocator())
    {
    }

public:
    virtual ~IScene() { }
    virtual bool construct(IRenderer*, VkCommandBuffer prepare_commands, int pass, VkBuffer staging_buffer, unsigned char* staging_data) = 0;
    virtual void handle_event(const SDL_Event&, SceneHost*) = 0;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, SceneHost*) = 0;
    virtual void record_commands(IRenderer*, uint32_t frame_number) = 0;

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass) = 0;
};

}
