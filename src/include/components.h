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
struct dirty_transform { };

}
