#version 450
#extension GL_EXT_scalar_block_layout : require

layout(std430, set = 0, binding = 0) uniform US0B0 {
    mat4 proj_view;
};

layout(std430, push_constant) uniform PC {
    vec4 translation_scale;
    vec4 rotation;
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

mat4 model() {
    mat4 T = mat4(1.0), R = mat4(1.0), S = mat4(1.0);
    T[3].xyz = translation_scale.xyz;
    S[0][0] = S[1][1] = S[2][2] = translation_scale.w;

    vec4 rot = normalize(rotation);
    float qxx = rot.x * rot.x;
    float qxy = rot.x * rot.y;
    float qxz = rot.x * rot.z;
    float qyy = rot.y * rot.y;
    float qyz = rot.y * rot.z;
    float qzz = rot.z * rot.z;
    float qwx = rot.w * rot.x;
    float qwy = rot.w * rot.y;
    float qwz = rot.w * rot.z;
    R[0][0] = 1.0 - 2.0 * (qyy + qzz);
    R[0][1] = 2.0 * (qxy + qwz);
    R[0][2] = 2.0 * (qxz - qwy);
    R[1][0] = 2.0 * (qxy - qwz);
    R[1][1] = 1.0 - 2.0 * (qxx +  qzz);
    R[1][2] = 2.0 * (qyz + qwx);
    R[2][0] = 2.0 * (qxz + qwy);
    R[2][1] = 2.0 * (qyz - qwx);
    R[2][2] = 1.0 - 2.0 * (qxx +  qyy);

    return T * R * S;
}

void main()
{
    gl_Position = proj_view * model() * vec4(in_position, 1.0);
    out_normal = in_normal;
    out_uv = in_uv;
}
