#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "asset.h"

namespace twogame::e_components {

struct camera { };
struct geometry {
    std::shared_ptr<asset::Mesh> m_mesh;
    std::shared_ptr<asset::Material> m_material;
    std::array<VkDescriptorSet, 2> m_descriptors;
    std::array<uint32_t, 2> m_descriptor_buffers;
};
struct hierarchy {
    entt::entity m_parent { entt::null };
    entt::entity m_child { entt::null };
    entt::entity m_prev { entt::null };
    entt::entity m_next { entt::null };
};

struct translation : glm::vec3 { };
struct orientation : glm::quat { };
struct transform : glm::mat4 { };
struct transform_dirty { };

struct morph_animation {
    std::shared_ptr<asset::Animation> m_animation, m_next_animation;
    uint64_t m_start_time;
    float m_multiplier;
};
struct morph_weights {
    std::vector<float> m_weights;
};
struct morph_weights_dirty_0 { };
struct morph_weights_dirty_1 { };

}
