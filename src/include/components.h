#pragma once
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include "asset.h"

namespace twogame::e_components {

struct geometry {
    std::shared_ptr<asset::Mesh> m_mesh;
    std::shared_ptr<asset::Material> m_material;
};

struct translation : glm::vec3 { };
struct orientation : glm::quat { };

}
