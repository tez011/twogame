#pragma once
#include <atomic>
#include <memory>
#include <queue>
#include <thread>
#include <unordered_map>
#include <SDL3/SDL_events.h>
#include <volk.h>
#include "core/mpmc.h"
#include "render/staging_buffer.h"

namespace twogame {

class IRenderer;
class IScene;

class SceneHost final {
    static std::unique_ptr<SceneHost> s_self;

    struct BQData {
        IScene* scene;
        bool bringup;
    };
    struct RQData {
        IScene* scene;
        uint64_t ticket;
        StagingBuffer* commands;
    };

    std::atomic<IScene*> m_active_scene;
    std::atomic_uint32_t m_frame_number;
    MPMCQ<BQData, 8> m_builder_queue;
    MPMCQ<RQData, 8> m_render_queue, m_return_queue;
    MPMCQ<SDL_Event, 64> m_event_queue;

    // Owned by scene thread
    using frame_number_t = uint32_t;
    IScene* m_requested_scene;
    std::thread m_scene_host;
    std::unordered_map<IScene*, uint64_t> m_scenes;
    std::queue<std::pair<IScene*, frame_number_t>> m_purge_queue;
    std::atomic_uint64_t m_max_ticket;
    bool m_active;

    // Owned by render thread
    std::unique_ptr<IRenderer> m_renderer;
    VkCommandPool m_xfer_command_pool, m_acquire_command_pool;
    VkSemaphore m_timeline;
    VkQueue m_graphics_queue, m_transfer_queue;

    // Owned by worker threads
    constexpr static int BUILDER_THREAD_COUNT = 2;
    std::array<std::thread, BUILDER_THREAD_COUNT> m_builders;
    std::array<StagingBuffer, BUILDER_THREAD_COUNT> m_staging_buffers;

    void scene_loop();
    void builder_loop(int thread_id);
    SceneHost(IRenderer* renderer, IScene* initial);

public:
    static void init(IRenderer* renderer, IScene* initial);
    static void drop();
    static SceneHost& owned()
    {
        return *s_self;
    }
    ~SceneHost();

    static inline IRenderer* renderer() { return s_self->m_renderer.get(); }

    /**
     * Enqueue the scene for preparation.
     * @warning only safe to call from the scene thread.
     * @return false if the queue is full and the caller should try again.
     */
    static bool prepare(IScene* scene);

    /**
     * Set the next scene. When this next scene is ready, the host will switch to it.
     * @warning only safe to call from the scene thread.
     */
    static void set_next_scene(IScene* scene);

    static void wait_frame(uint32_t frame_number);
    static void push_event(SDL_Event*);
    static void submit_transfers();

    static void execute_draws(VkCommandBuffer container, uint32_t frame_number, int subpass);
};

}
