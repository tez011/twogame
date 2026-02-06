#version 450

layout(set = 1, binding = 0) uniform sampler2D base_color_texture;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 base_color = vec4(texture(base_color_texture, in_uv).xyz, 1.0);
    float lighting = 0.3 + clamp(1.5 * dot(in_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(base_color.xyz * lighting, 1.0);
}
