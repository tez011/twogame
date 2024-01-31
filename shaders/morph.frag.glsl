#version 450

layout(set = 3, binding = 0) uniform MaterialUniformData {
    vec3 base_color;
};

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(base_color, 1.0);
}
