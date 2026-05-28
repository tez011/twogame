#version 450

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba32f) uniform readonly image2D source_image;
layout(binding = 1, rgba32f) uniform writeonly image2D dest_image;
const float EPS = 0.00001, PI = 3.14159265358979323;

float sinc(float x)
{
    if (abs(x) < EPS)
        return 1.0;
    else
        return sin(PI * x) / (PI * x);
}

float lanczos_weight(float a, float x)
{
    if (x <= a)
        return sinc(x) * sinc(x / a);
    else
        return 0.0;
}

void main()
{
    // Sample some neighboring pixels
    uvec2 source_size = imageSize(source_image);
    vec4 total_samples = vec4(0, 0, 0, 0);
    float sample_count = 0.0;
    for (int dx = -2; dx <= 3; ++dx) {
        for (int dy = -2; dy <= 3; ++dy) {
            ivec2 pp = 2 * ivec2(gl_GlobalInvocationID.xy) + ivec2(dx, dy);
            if (0 <= pp.x && pp.x < source_size.x && 0 <= pp.y && pp.y < source_size.y) {
                vec4 pixel = imageLoad(source_image, pp);
                float w = lanczos_weight(4.0, length(vec2(dx - 0.5, dy - 0.5)));
                total_samples += w * pixel;
                sample_count += w;
            }
        }
    }

    vec4 out_color = clamp(total_samples / sample_count, 0.0, 1.0);
    imageStore(dest_image, ivec2(gl_GlobalInvocationID.xy), out_color);
}
