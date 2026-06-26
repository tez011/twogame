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
    uint32_t mesh_id;
    uint32_t material_id;
};
struct MeshEntry {
    VkDeviceAddress vertex_buffer_address;
    VkDeviceAddress normal_buffer_address;
    VkDeviceAddress index_buffer_address;
};

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

}
