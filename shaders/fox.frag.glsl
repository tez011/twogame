#version 450

layout(set = 3, binding = 1) uniform sampler2D base_color_texture;

layout(location = 0) in vec2 frag_uv0;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(base_color_texture, frag_uv0);
}
