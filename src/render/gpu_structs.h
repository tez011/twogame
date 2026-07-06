#pragma once
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

}
