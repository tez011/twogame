#version 450
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require

struct Material {
    vec4 base_color_factor;
    vec3 emissive_factor;
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

layout(set = 0, binding = 2) readonly buffer MaterialBuffer { Material materials[]; };
layout(set = 0, binding = 3) uniform sampler global_samplers[1];
layout(set = 0, binding = 4) uniform texture2D picture_book[];

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 2) flat in uint in_material_id;
layout(location = 0) out vec4 out_color;

void main()
{
    Material mtl = materials[in_material_id];

    vec4 base_color = vec4(texture(sampler2D(picture_book[nonuniformEXT(mtl.base_color_texture)], global_samplers[0]), in_uv).xyz, 1.0);
    float lighting = 0.2 + clamp(1.5 * dot(normalize(in_normal), normalize(vec3(0.3, 1.0, 0.4))), 0.0, 0.8);
    out_color = vec4(base_color.xyz * lighting, 1.0);
}
