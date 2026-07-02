#pragma once
#include <atomic>
#include <functional>
#include <queue>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <SDL3/SDL_events.h>
#include <volk.h>
#include "constants.h"
#include "mpmc.h"
#include "structs.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IAsset;
class IRenderer;
namespace asset {
    class Animation;
    class Image;
    class Mesh;
    class Skeleton;
}
namespace fbs {
    struct Assets;
}

class StagingBuffer {
    friend class SceneHost;

    VkBuffer m_src_buffer;
    VmaAllocation m_src_mem;
    std::span<std::byte> m_src_data;
    VkCommandBuffer m_xfer_commands, m_acquire_commands;
    VkSemaphore m_post_xfer;
    VkDeviceSize m_tail;

    std::vector<VkBufferMemoryBarrier2> m_buffer_memory_barriers;
    std::vector<std::pair<VkCopyBufferInfo2, std::vector<VkBufferCopy2>>> m_buffer_copies;
    std::array<std::vector<VkImageMemoryBarrier2>, 2> m_image_memory_barriers;
    std::vector<std::pair<VkCopyBufferToImageInfo2, std::vector<VkBufferImageCopy2>>> m_image_copies;

public:
    StagingBuffer()
        : m_src_buffer(VK_NULL_HANDLE)
        , m_src_mem(VK_NULL_HANDLE)
        , m_tail(0)
    {
    }
    constexpr inline VkDeviceSize size() const { return Constants::STAGING_BUFFER_SIZE; }
    inline VkDeviceSize tail_offset() const { return m_tail; }
    inline std::byte* tail() { return m_src_data.subspan(m_tail).data(); }
    void advance(size_t);

    void copy_image(VkImage dst, const VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout);
    void copy_buffer(VkBuffer dst, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

    /**
     * @warning It is illegal to do any operations on this object until this staging buffer has been submitted to a queue,
     * and the host has been signaled that that operation is complete.
     */
    void finalize();
};

class AssetContainer {
protected:
    std::vector<std::shared_ptr<asset::Animation>> m_animations;
    std::vector<std::shared_ptr<asset::Image>> m_images;
    std::vector<std::shared_ptr<asset::Mesh>> m_meshes;
    std::vector<std::shared_ptr<asset::Skeleton>> m_skeletons;

public:
    virtual ~AssetContainer() { }
    AssetContainer& operator+=(const AssetContainer& other);

    std::vector<std::shared_ptr<asset::Animation>>& animations() { return m_animations; }
    std::span<const std::shared_ptr<asset::Animation>> animations() const { return m_animations; }
    std::vector<std::shared_ptr<asset::Image>>& images() { return m_images; }
    std::span<const std::shared_ptr<asset::Image>> images() const { return m_images; }
    std::vector<std::shared_ptr<asset::Mesh>>& meshes() { return m_meshes; }
    std::span<const std::shared_ptr<asset::Mesh>> meshes() const { return m_meshes; }
    std::vector<std::shared_ptr<asset::Skeleton>>& skeletons() { return m_skeletons; }
    std::span<const std::shared_ptr<asset::Skeleton>> skeletons() const { return m_skeletons; }
};

class SceneGraph {
    std::vector<uint32_t> m_parents, m_children, m_siblings;
    std::vector<std::variant<mat4s, TRS>> m_local_transforms;
    std::vector<mat4s> m_global_transforms;

public:
    constexpr static uint32_t NONE = std::numeric_limits<uint32_t>::max();

    SceneGraph() { }
    SceneGraph(std::vector<uint32_t>&& parents, std::vector<std::variant<mat4s, TRS>>&& transforms);
    inline size_t node_count() const { return m_parents.size(); }
    inline std::span<const mat4s> global_transforms() const { return m_global_transforms; }
    TRS& transform(uint32_t node);

    /** @return the index of the newly inserted, empty scene node */
    uint32_t insert(uint32_t parent, const std::variant<mat4s, TRS>& xfm);
    /** Inserts an `other` scene graph into this one, preserving node order in both.
     * @return the index of the first node of the inserted scene graph. */
    uint32_t insert(uint32_t parent, const SceneGraph& other);
    void reparent(uint32_t node, uint32_t new_parent);
    /** Erases a node from the scene without modifying existing elements.
     * No memory is reclaimed. */
    void remove(uint32_t node);

    void update_global_transforms();
};

class SceneManifest {
    std::shared_ptr<const fbs::Assets> m_manifest;
    AssetContainer m_container;
    SceneGraph m_scenegraph;
    std::vector<CameraNode> m_cameras;
    std::vector<MeshNode> m_meshes;
    std::function<bool(size_t, std::function<void*(size_t)>)> m_slurp_buffer;

public:
    SceneManifest(const std::string& path);
    inline std::shared_ptr<const fbs::Assets> manifest() const { return m_manifest; }
    inline const AssetContainer& assets() const { return m_container; }
    inline const SceneGraph& scene() const { return m_scenegraph; }
    inline std::span<const CameraNode> cameras() const { return m_cameras; }
    inline std::span<const MeshNode> meshes() const { return m_meshes; }

    template <typename T = std::byte>
    std::vector<T> buffer(size_t i) const
    {
        std::vector<T> out;
        auto resize = [&out](size_t nsz) -> void* {
            out.resize((nsz + sizeof(T) - 1) / sizeof(T));
            return out.data();
        };
        SDL_assert(m_slurp_buffer(i, resize));
        return out;
    }
    template <typename T = std::byte>
    std::function<std::vector<T>(size_t)> buffer_resolver() const
    {
        return [slurp = this->m_slurp_buffer](size_t i) -> std::vector<T> {
            std::vector<T> out;
            auto resize = [&out](size_t nsz) -> void* {
                out.resize((nsz + sizeof(T) - 1) / sizeof(T));
                return out.data();
            };
            SDL_assert(slurp(i, resize));
            return out;
        };
    }
    using BufferResolver = std::function<std::vector<std::byte>(size_t)>;
};

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
    constexpr static int FRAMES_IN_FLIGHT = Constants::FRAMES_IN_FLIGHT;
    AssetContainer m_assets;
    SceneGraph m_scenegraph;
    std::vector<CameraNode> m_cameras;
    std::vector<MeshNode> m_draw_meshes;
    std::vector<uint32_t> m_sparse_meshes;
    std::vector<AnimationInstance> m_animations;

    VkDescriptorPool m_descriptor_pool;
    std::array<std::array<VkDescriptorSet, 1>, FRAMES_IN_FLIGHT> m_descriptor_set;

    std::span<BindingZero> m_binding_zero;
    std::span<MeshEntry> m_mesh_refs;
    std::array<std::span<InstanceEntry>, FRAMES_IN_FLIGHT> m_instances;
    std::array<std::span<VkDrawIndirectCommand>, FRAMES_IN_FLIGHT> m_draw_commands;

    IScene();

private:
    struct ManagedBuffer {
        VkBuffer handle;
        VmaAllocation mem;
        VkMemoryPropertyFlags memflags;
    };

    std::array<VkCommandPool, FRAMES_IN_FLIGHT> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, FRAMES_IN_FLIGHT> m_draw_cmd;
    ManagedBuffer m_vertices_buffer, m_mesh_refs_buffer, m_binding_zero_buffer, m_instances_buffer[FRAMES_IN_FLIGHT], m_indirect_buffer[FRAMES_IN_FLIGHT];
    std::span<std::byte> m_vertices_ptr;

public:
    virtual ~IScene();
    inline std::span<const MeshEntry> mesh_references() const { return m_mesh_refs; }
    inline VkBuffer mesh_data_buffer() const { return m_vertices_buffer.handle; }
    inline std::span<std::byte> mesh_data_pointer() const { return m_vertices_ptr; }
    inline VkCommandBuffer draw_commands(uint32_t frame_number, int subpass) const { return m_draw_cmd[frame_number % FRAMES_IN_FLIGHT][subpass]; }

    virtual void handle_event(const SDL_Event&, SceneHost*) = 0;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, SceneHost*) = 0;
    virtual void render(IRenderer*, uint32_t frame_number) = 0;

    std::vector<std::vector<IAsset*>> begin_construct_assets(IRenderer*, StagingBuffer& commands);
    void end_construct_assets(IRenderer*);
    void record_commands(IRenderer*, uint32_t frame_number);
};

}
