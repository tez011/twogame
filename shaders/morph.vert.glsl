#version 450

layout(set = 1, binding = 0) uniform DS1B0 {
    mat4 proj;
    mat4 view;
};
layout(set = 2, binding = 0) uniform ModelMat {
    mat4 model;
};
layout(set = 2, binding = 2) uniform MorphWeights {
    vec4 morph_weights[16];
};
layout(set = 2, binding = 3) uniform sampler2D morph_position_displacements;
layout(set = 2, binding = 4) uniform sampler2D morph_normal_displacements;
layout(push_constant) uniform PushConstants {
    uint first_vertex;
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec2 in_uv1;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv0;
layout(location = 2) out vec2 frag_uv1;

void main()
{
    vec3 displaced_position = in_position;
    for (uint i = 0; i < textureSize(morph_position_displacements, 0).y; i++) {
        displaced_position += morph_weights[i / 4][i % 4] * texelFetch(morph_position_displacements, ivec2(gl_VertexIndex + first_vertex, i), 0).xyz;
    }

    frag_normal = in_normal;
    for (uint i = 0; i < textureSize(morph_normal_displacements, 0).y; i++) {
        frag_normal += morph_weights[i / 4][i % 4] * texelFetch(morph_normal_displacements, ivec2(gl_VertexIndex + first_vertex, i), 0).xyz;
    }

    gl_Position = proj * view * model * vec4(displaced_position, 1.0);
    frag_uv0 = in_uv0;
    frag_uv1 = in_uv1;
}
