#version 450

layout(set = 3, binding = 0) uniform MaterialUniformData {
    vec4 base_color_factor;
};

layout(location = 0) in vec3 frag_normal;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 mtl_color = base_color_factor;
    float lighting = 0.3 + clamp(1.5 * dot(frag_normal, vec3(1.0, 0.0, 0.0)), 0.0, 0.7);
    out_color = vec4(mtl_color.xyz * lighting, 1.0);
}
