#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform sampler global_samplers[1];
layout(set = 0, binding = 2) uniform texture2D picture_book[];

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 2) flat in uint in_material_id;
layout(location = 0) out vec4 out_color;

void main()
{
    uint texture_index = in_material_id; // TODO structure when material has more info

    vec4 base_color = vec4(texture(sampler2D(picture_book[nonuniformEXT(texture_index)], global_samplers[0]), in_uv).xyz, 1.0);
    float lighting = 0.3 + clamp(1.5 * dot(in_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(base_color.xyz * lighting, 1.0);
}
