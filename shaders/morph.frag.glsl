#version 450

layout(set = 3, binding = 0) uniform sampler2D base_color_texture;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv0;
layout(location = 2) in vec2 frag_uv1;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 mtl_color = texture(base_color_texture, frag_uv0);
    float lighting = 0.3 + clamp(1.5 * dot(frag_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(mtl_color.xyz * lighting, mtl_color.a);
}
