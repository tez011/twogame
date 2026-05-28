#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform BindingZero {
    mat4 proj;
    mat4 view;
};

struct Vertex {
    vec3 position;
    vec2 uv[2];
};

struct Normal {
    vec3 normal;
    vec3 tangent;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer { Vertex verts[]; };
layout(buffer_reference, std430) readonly buffer NormalBuffer { Normal normals[]; };
layout(buffer_reference, std430) readonly buffer IndexBuffer { uint16_t index[]; };

struct Instance {
    uint64_t vertex_buffer;
    uint64_t normal_buffer;
    uint64_t index_buffer;
    uint32_t material_id;
    mat4 model;
};

layout(buffer_reference, std430) readonly buffer InstanceBuffer {
    Instance instances[];
};

layout(push_constant, std430) uniform PC {
    uint64_t instance_buffer_address;
};

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;
layout(location = 2) flat out uint out_material_id;

void main()
{
    InstanceBuffer instance_buffer = InstanceBuffer(instance_buffer_address);
    Instance instance = instance_buffer.instances[gl_InstanceIndex];
    VertexBuffer vertex_buffer = VertexBuffer(instance.vertex_buffer);
    NormalBuffer normal_buffer = NormalBuffer(instance.normal_buffer);
    IndexBuffer index_buffer = IndexBuffer(instance.index_buffer);

    uint vertex_index = uint(index_buffer.index[gl_VertexIndex]);
    Vertex v = vertex_buffer.verts[vertex_index];
    Normal n = normal_buffer.normals[vertex_index];

    gl_Position = proj * view * instance.model * vec4(v.position, 1.0);
    out_normal = n.normal;
    out_uv = v.uv[0];
    out_material_id = instance.material_id;
}
