#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(set = 0, binding = 0) uniform PerFrameData {
    mat4 proj;
    mat4 view;
};

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

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

void main()
{
    gl_Position = proj * view * model.model[gl_InstanceIndex] * vec4(in_position, 1.0);
    out_normal = in_normal;
    out_uv = in_uv;
}
