#pragma once
#include <array>
#include <vector>
#include <SDL3/SDL_events.h>
#include <volk.h>
#include "assets/animation.h"
#include "core/constants.h"
#include "render/gpu_structs.h"
#include "scene/scene_manifest.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IRenderer;
class SceneHost;

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
    bool m_cameras_dirty = true, m_draw_meshes_dirty = true;

    VkDescriptorPool m_descriptor_pool;
    std::array<std::array<VkDescriptorSet, 1>, FRAMES_IN_FLIGHT> m_descriptor_set;

    std::span<BindingZero> m_binding_zero;
    std::array<std::span<vec4s>, FRAMES_IN_FLIGHT> m_varying;
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
    ManagedBuffer m_vertices_buffer, m_mesh_refs_buffer, m_materials_buffer, m_binding_zero_buffer;
    std::array<ManagedBuffer, FRAMES_IN_FLIGHT> m_instances_buffer {}, m_indirect_buffer {}, m_varying_buffer {};
    std::span<std::byte> m_vertices_ptr;
    std::span<MeshEntry> m_mesh_refs;
    std::span<MaterialEntry> m_material_ptr;

    // These are implemented by derived scenes
    virtual void handle_event(const SDL_Event&, SceneHost*) { }
    virtual void logic(uint64_t frame_number, uint64_t frame_time, uint64_t delta_time, SceneHost*) { }

    void animate(uint64_t frame_time);
    void record_commands(IRenderer*, uint32_t frame_number);

public:
    virtual ~IScene();
    inline VkBuffer material_entries_buffer() const { return m_materials_buffer.handle; }
    inline std::span<MaterialEntry> material_entries() const { return m_material_ptr; }
    inline std::span<const MeshEntry> mesh_entries() const { return m_mesh_refs; }
    inline VkBuffer mesh_data_buffer() const { return m_vertices_buffer.handle; }
    inline std::span<std::byte> mesh_data_pointer() const { return m_vertices_ptr; }
    inline VkCommandBuffer draw_commands(uint32_t frame_number, int subpass) const { return m_draw_cmd[frame_number % FRAMES_IN_FLIGHT][subpass]; }

    std::vector<std::vector<IAsset*>> begin_construct_assets(IRenderer*, StagingBuffer& commands);
    void end_construct_assets(IRenderer*);
    void handle_base_event(const SDL_Event&, SceneHost*);
    void tick(uint64_t frame_number, uint64_t frame_time, uint64_t delta_time, SceneHost*);
};

}
