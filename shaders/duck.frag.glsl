#version 450

layout(set = 3, binding = 1) uniform sampler2D base_color_texture;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_tex_coord;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 mtl_color = texture(base_color_texture, frag_tex_coord);
    float lighting = 0.3 + clamp(1.5 * dot(frag_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(mtl_color.xyz * lighting, 1.0);
}
