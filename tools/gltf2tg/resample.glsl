#version 450

layout(local_size_x = 8, local_size_y = 8) in;
layout(constant_id = 0) const uint FILTER_TYPE = 0;
layout(binding = 0, rgba32f) uniform readonly image2D in_image;
layout(binding = 1, rgba32f) uniform writeonly image2D out_image;
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
    uvec2 in_size = imageSize(in_image), out_size = imageSize(out_image);
    ivec2 out_coords = ivec2(gl_GlobalInvocationID.xy);
    vec4 out_color = vec4(0.0);

    if (FILTER_TYPE == 0) {
        // lanczos filtering, for color/material
        vec4 total_samples = vec4(0.0);
        float sample_count = 0.0;
        for (int dx = -2; dx <= 3; ++dx) {
            for (int dy = -2; dy <= 3; ++dy) {
                ivec2 pp = 2 * out_coords + ivec2(dx, dy);
                if (0 <= pp.x && pp.x < in_size.x && 0 <= pp.y && pp.y < in_size.y) {
                    vec4 pixel = imageLoad(in_image, pp);
                    float w = lanczos_weight(4.0, length(vec2(dx - 0.5, dy - 0.5)));
                    total_samples += w * pixel;
                    sample_count += w;
                }
            }
        }

        out_color = clamp(total_samples / sample_count, 0.0, 1.0);
    } else if (FILTER_TYPE == 1) {
        // vector bilinear filtering, for normal/tangent

        vec3 total_vector = vec3(0.0);
        float total_alpha = 0.0;
        for (int dx = 0; dx < 2; ++dx) {
            for (int dy = 0; dy < 2; ++dy) {
                ivec2 pp = 2 * out_coords + ivec2(dx, dy);
                pp.x = min(pp.x, int(in_size.x) - 1);
                pp.y = min(pp.y, int(in_size.y) - 1);
                vec4 pc = imageLoad(in_image, pp);
                total_vector += (pc.rgb * 2.0) - vec3(1.0);
                total_alpha += pc.a;
            }
        }

        float len = length(total_vector);
        if (len < EPS) {
            total_vector = vec3(0.0, 1.0, 0.0);
        } else {
            total_vector /= len;
        }

        out_color = vec4(total_vector * 0.5 + vec3(0.5), clamp(total_alpha / 4.0, 0.0, 1.0));
    }
    imageStore(out_image, out_coords, out_color);
}
