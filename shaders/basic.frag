#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 2, binding = 0) uniform sampler2D picture_book[];

layout(buffer_reference, std430) buffer PerObjectData {
    uint nothing;
};

layout(buffer_reference, std430) buffer Models {
    mat4 model[];
};

layout(buffer_reference, std430) buffer MaterialInfo {
    uint base_color_texture;
};

layout(std430, push_constant) uniform PC {
    PerObjectData object;
    Models model;
    MaterialInfo material;
};

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 base_color = vec4(texture(picture_book[material.base_color_texture], in_uv).xyz, 1.0);
    float lighting = 0.3 + clamp(1.5 * dot(in_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(base_color.xyz * lighting, 1.0);
}
