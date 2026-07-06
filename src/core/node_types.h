#pragma once
#include <cstdint>
#include <memory>

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
    uint32_t sibling_dense_mesh;
    std::shared_ptr<uint32_t[]> skin;
    std::shared_ptr<float[]> weights;
};
