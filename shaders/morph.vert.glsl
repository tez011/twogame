#version 450

layout(set = 1, binding = 0) uniform DS1B0 {
    mat4 proj;
    mat4 view;
};
layout(set = 2, binding = 0) uniform sampler1DArray morph_position_displacements;
layout(set = 2, binding = 1) uniform MorphPositionWeights {
    vec4 morph_position_weights[16];
};
layout(push_constant) uniform PushConstants {
    mat4 model;
};

layout(location = 0) in vec3 in_position;

void main()
{
    vec3 displaced_position = in_position;
    for (uint i = 0; i < textureSize(morph_position_displacements, 0).y; i++) {
        displaced_position += morph_position_weights[i / 4][i % 4] * texelFetch(morph_position_displacements, ivec2(gl_VertexIndex, i), 0).xyz;
    }

    gl_Position = proj * view * model * vec4(displaced_position, 1.0);
}
