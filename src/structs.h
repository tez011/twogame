#pragma once
#include <cstdint>
#include <memory>
#include <variant>
#include <cglm/struct.h>
#include <volk.h>

namespace twogame {

struct BindingZero {
    mat4s proj;
    mat4s view;
};
struct InstanceEntry {
    mat4s model;
    VkDeviceAddress joint_matrices;
    VkDeviceAddress morph_weights;
    uint32_t mesh_id;
    uint32_t material_id;
};
struct MeshEntry {
    VkDeviceAddress index_buffer_address;
    VkDeviceAddress vertex_buffer_address;
    VkDeviceAddress normal_buffer_address;
    VkDeviceAddress joints_buffer_address;
    VkDeviceAddress vertex_displacement_address;
    VkDeviceAddress normal_displacement_address;
    uint32_t joint_count;
    uint32_t displacement_count;
};
struct MaterialEntry {
    vec4s base_color_factor;
    vec3s emissive_factor;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;

    uint16_t base_color_texture;
    uint16_t metallic_roughness_texture;
    uint16_t normal_texture;
    uint16_t occlusion_texture;
    uint16_t base_color_uv;
    uint16_t metallic_roughness_uv;
    uint16_t normal_uv;
    uint16_t occlusion_uv;
    uint16_t emissive_uv;
};
constexpr auto a = sizeof(MaterialEntry);

struct TRS {
    versors rotation;
    vec3s translation;
    vec3s scale;

    TRS()
        : rotation(GLMS_VEC4_BLACK_INIT)
        , translation(GLMS_VEC3_ZERO_INIT)
        , scale(GLMS_VEC3_ONE_INIT)
    {
    }

    mat4s transform_matrix() const;
};

struct SceneNode {
    uint32_t node_index;
};
struct CameraNode : public SceneNode {
    uint32_t camera_index;
};
struct MeshNode : public SceneNode {
    uint32_t mesh_index;
    uint32_t material_index;
    uint32_t skeleton_index;
    std::shared_ptr<uint32_t[]> skin;
    std::shared_ptr<float[]> weights;
};

struct AnimationTarget {
    struct Field {
        enum {
            Translation = 1,
            Rotation,
            Scale,
            Weights,
        };
    };
    uint32_t object;
    uint16_t width;
    union {
        uint16_t _uv;
        struct {
            unsigned field : 4;
            unsigned object_is_bone : 1;
        };
    };
};
struct AnimationInstance {
    uint64_t start_time;
    uint32_t animation_index;
    std::unique_ptr<uint32_t[]> keyframe_hints;
    std::variant<std::monostate, // Use the node target from the animation asset
        std::unique_ptr<uint32_t[]>, // We specify our own node target
        std::weak_ptr<uint32_t[]> // This is a skeletal animation, and these are the skin nodes
        >
        custom_targets;
    bool loop;
};

}
