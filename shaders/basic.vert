#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

struct Vertex {
    vec3 position;
    vec2 uv[2];
};

struct Normal {
    vec3 normal;
    vec3 tangent;
    vec4 color;
};

struct Mesh {
    uint64_t index_buffer;
    uint64_t vertex_buffer;
    uint64_t normal_buffer;
    uint64_t joints_buffer;
    uint64_t vertex_displacements;
    uint64_t normal_displacements;
    uint32_t joint_count;
    uint32_t displacement_count;
};

struct Instance {
    mat4 model;
    uint64_t joint_matrices;
    uint64_t morph_weights;
    uint32_t mesh_id;
    uint32_t material_id;
    uint32_t padding[2];
};

layout(buffer_reference, std430) readonly buffer VertexBuffer { Vertex verts[]; };
layout(buffer_reference, std430) readonly buffer NormalBuffer { Normal normals[]; };
layout(buffer_reference, std430) readonly buffer IndexBuffer { uint16_t index[]; };
layout(buffer_reference, std430) readonly buffer InstanceBuffer { Instance instances[]; };
layout(buffer_reference, std430) readonly buffer DenseMat4Buffer { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer DenseFloatBuffer { float f[]; };
layout(buffer_reference, std430) readonly buffer DenseUintBuffer { uint32_t i[]; };

layout(set = 0, binding = 0) uniform BindingZero {
    mat4 proj;
    mat4 view;
};

layout(set = 0, binding = 1, std430) readonly buffer MeshBuffer { Mesh meshes[]; };

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
    Mesh mesh = meshes[instance.mesh_id];
    IndexBuffer index_buffer = IndexBuffer(mesh.index_buffer);
    VertexBuffer vertex_buffer = VertexBuffer(mesh.vertex_buffer);
    NormalBuffer normal_buffer = NormalBuffer(mesh.normal_buffer);
    DenseUintBuffer joint_indexes = DenseUintBuffer(mesh.joints_buffer);
    DenseFloatBuffer joint_weights = DenseFloatBuffer(mesh.joints_buffer);

    uint vertex_index = uint(index_buffer.index[gl_VertexIndex]);
    Vertex v = vertex_buffer.verts[vertex_index];
    Normal n = normal_buffer.normals[vertex_index];
    vec3 position = v.position, normal = n.normal;
    vec2 uv[2] = v.uv;

    if (instance.morph_weights != 0) {
        DenseFloatBuffer morph_weights = DenseFloatBuffer(instance.morph_weights);
        VertexBuffer morph_posns = VertexBuffer(mesh.vertex_displacements);
        NormalBuffer morph_norms = NormalBuffer(mesh.normal_displacements);
        for (uint i = 0; i < mesh.displacement_count; i++) {
            Vertex morph_posn = morph_posns.verts[vertex_index * mesh.displacement_count + i];
            Normal morph_norm = morph_norms.normals[vertex_index * mesh.displacement_count + i];
            position += morph_weights.f[i] * morph_posn.position;
            uv[0] += morph_weights.f[i] * morph_posn.uv[0];
            uv[1] += morph_weights.f[i] * morph_posn.uv[1];
            normal += morph_weights.f[i] * morph_norm.normal;
        }
    }

    mat4 skin;
    if (instance.joint_matrices != 0) {
        DenseMat4Buffer joint_mats = DenseMat4Buffer(instance.joint_matrices);
        skin = mat4(0.0);
        for (uint i = 0; i < mesh.joint_count; i++) {
            float weight = joint_weights.f[vertex_index * mesh.joint_count * 2 + mesh.joint_count + i];
            skin += weight * joint_mats.m[joint_indexes.i[vertex_index * mesh.joint_count * 2 + i]];
        }
    } else {
        skin = mat4(1.0);
    }

    gl_Position = proj * view * instance.model * skin * vec4(position, 1.0);
    out_normal = normal;
    out_uv = uv[0];
    out_material_id = instance.material_id;
}
