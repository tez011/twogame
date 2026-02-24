#pragma once
#include <atomic>
#include <queue>
#include <set>
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
    static std::unique_ptr<SceneHost> s_self;

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
        StagingBuffer() { }
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

class IScene {
    friend class SceneHost;

protected:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;

    IScene() { }

public:
    virtual ~IScene() { }
    virtual bool construct(IRenderer* renderer, SceneHost::StagingBuffer& buffer, size_t pass, size_t ticket) = 0;
    virtual void handle_event(const SDL_Event&, SceneHost*) = 0;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, SceneHost*) = 0;
    virtual void record_commands(IRenderer*, uint32_t frame_number) = 0;

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass) = 0;
};

class IAsset {
public:
    enum class Type {
        Image,
        Material,
        Mesh,
    };

protected:
    std::variant<std::shared_ptr<void>, uint64_t> m_prepared;

    IAsset() { }

public:
    virtual ~IAsset() { }
    virtual Type type() const = 0;

    virtual void push_dependents(std::queue<IAsset*>&) const { }
    virtual size_t prepare_needs() const = 0;
    virtual size_t prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) = 0;
    void post_prepare(uint64_t ready);
};

namespace asset {

    class Image final : public IAsset {
        VkImage m_image;
        VmaAllocation m_mem;
        VkImageView m_image_view;

    public:
        Image();
        ~Image();
        inline virtual Type type() const override { return IAsset::Type::Image; }
        inline VkImage handle() const { return m_image; }
        inline VkImageView view() const { return m_image_view; }

        virtual size_t prepare_needs() const override;
        virtual size_t prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) override;
    };

    class Material : public IAsset {
        std::shared_ptr<Image> m_base_color_texture;

    public:
        Material();
        ~Material();
        inline virtual Type type() const override { return IAsset::Type::Material; }

        virtual void push_dependents(std::queue<IAsset*>&) const override;
        virtual size_t prepare_needs() const override;
        virtual size_t prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) override;

        Image* base_color_texture() const { return m_base_color_texture.get(); }
    };

    class Mesh final : public IAsset {
    public:
        VkBuffer m_vertex_buffer;
        VkBuffer m_index_buffer;
        VmaAllocation m_vertex_mem, m_index_mem;
        std::vector<std::shared_ptr<Material>> m_materials;

    public:
        Mesh();
        ~Mesh();
        inline virtual Type type() const override { return IAsset::Type::Mesh; }

        virtual void push_dependents(std::queue<IAsset*>&) const override;
        virtual size_t prepare_needs() const override;
        virtual size_t prepare(SceneHost::StagingBuffer& commands, VkDeviceSize offset) override;
    };

}

}
