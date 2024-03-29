#pragma once
#include <cglm/struct.h>
#include <entt/entt.hpp>
#include "asset.h"
#include "render.h"

namespace twogame::e_components {

struct camera { };
struct geometry {
    std::shared_ptr<asset::Mesh> m_mesh;
    std::shared_ptr<asset::Skeleton> m_skeleton;
    std::vector<std::shared_ptr<asset::Material>> m_materials;
    std::array<VkDescriptorSet, 2> m_descriptors;
    Renderer::perobject_descriptor_buffers_t m_descriptor_buffers;
};
struct hierarchy {
    entt::entity m_parent { entt::null };
    entt::entity m_child { entt::null };
    entt::entity m_prev { entt::null };
    entt::entity m_next { entt::null };
};

typedef vec3s translation;
typedef versors orientation;
typedef mat4s transform;
struct transform_dirty { };
struct transform_dirty_0 { };
struct transform_dirty_1 { };

struct animation {
    std::shared_ptr<asset::Animation> m_animation, m_next_animation;
    uint64_t m_start_time;
    float m_multiplier;
};

struct morph_weights {
    std::vector<float> m_weights;
};
struct morph_weights_dirty_0 { };
struct morph_weights_dirty_1 { };

struct bone {
    entt::entity m_ancestor;
};
struct joints {
    std::vector<entt::entity> m_bones;
};
struct joint_mats {
    std::vector<mat4s> m_mats; // globalJointTransform * inverseBindMatrix
};
struct joint_mats_dirty_0 { };
struct joint_mats_dirty_1 { };

}
