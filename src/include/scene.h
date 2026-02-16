#pragma once
#include <atomic>
#include <list>
#include <queue>
#include <span>
#include <stack>
#include <thread>
#include <unordered_map>
#include <variant>
#include "display.h"
#include "mpmc.h"

namespace twogame {

class IScene;

class SceneHost final {
public:
    constexpr static VkDeviceSize STAGING_BUFFER_SIZE = 1 << 29;
    class StagingBuffer {
        friend class SceneHost;

        VkBuffer m_src_buffer;
        VmaAllocation m_src_mem;
        std::span<std::byte> m_src_data;
        VkCommandBuffer m_xfer_commands, m_acquire_commands;
        VkSemaphore m_post_xfer;

        std::vector<VkBufferMemoryBarrier2> m_buffer_memory_barriers;
        std::vector<std::pair<VkCopyBufferInfo2, std::vector<VkBufferCopy2>>> m_buffer_copies;
        std::array<std::vector<VkImageMemoryBarrier2>, 2> m_image_memory_barriers;
        std::vector<std::pair<VkCopyBufferToImageInfo2, std::vector<VkBufferImageCopy2>>> m_image_copies;

    public:
        StagingBuffer() {}
        inline std::span<std::byte> window(VkDeviceSize offset) const { return m_src_data.subspan(offset); }
        void copy_image(VkImage dst, VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout);
        void copy_buffer(VkBuffer dst, VkDeviceSize dst_size, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
        void finalize();
    };

private:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;
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
    std::atomic_uint32_t m_frame_number = 0;
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
    IRenderer* r_renderer;
    VkCommandPool m_xfer_command_pool, m_acquire_command_pool;
    VkSemaphore m_timeline;
    VkQueue m_graphics_queue, m_transfer_queue;

    // Owned by worker threads
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

    IScene() { }

public:
    virtual ~IScene() { }
    virtual bool construct(IRenderer*, int pass, SceneHost::StagingBuffer&) = 0;
    virtual void handle_event(const SDL_Event&, SceneHost*) = 0;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, SceneHost*) = 0;
    virtual void record_commands(IRenderer*, uint32_t frame_number) = 0;

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass) = 0;
};

class IAsset {
    static std::unordered_map<uint64_t, std::weak_ptr<IAsset>> s_cache;

protected:
    std::variant<std::shared_ptr<void>, uint64_t> m_prepared;

    IAsset() { }

public:
    virtual ~IAsset() { }

    virtual size_t prepare_needs() const = 0;
    virtual void prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) = 0;
    void post_prepare(uint64_t ready);
    virtual std::list<IAsset*> dependencies() const { return {}; }
};

namespace asset {

    class Image final : public IAsset {
        VkImage m_image;
        VmaAllocation m_mem;
        VkImageView m_image_view;

    public:
        Image();
        ~Image();
        inline operator VkImage() const { return m_image; }
        inline VkImageView view() const { return m_image_view; }

        virtual size_t prepare_needs() const override;
        virtual void prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) override;
    };

}

}
