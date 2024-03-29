#version 450

layout(set = 1, binding = 0) uniform DS1B0 {
    mat4 proj;
    mat4 view;
};
layout(set = 2, binding = 0) uniform DS2B0 {
    mat4 model;
};
layout(set = 2, binding = 1) uniform JointMatrices {
    mat4 joint_mat[32];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in uvec4 in_joints;
layout(location = 3) in vec4 in_weights;

layout(location = 0) out vec3 frag_normal;

void main()
{
    mat4 skin = in_weights.x * joint_mat[in_joints.x] +
        in_weights.y * joint_mat[in_joints.y] +
        in_weights.z * joint_mat[in_joints.z] +
        in_weights.w * joint_mat[in_joints.w];
    gl_Position = proj * view * model * skin * vec4(in_position, 1.0);
    frag_normal = in_normal;
}
