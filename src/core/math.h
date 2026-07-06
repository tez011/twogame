#pragma once
#include <cglm/struct.h>

namespace twogame {

struct TRS {
    versors rotation;
    vec3s translation;
    vec3s scale;

    TRS()
        : rotation(GLMS_VEC4_BLACK_INIT)
        , translation(GLMS_VEC3_ZERO_INIT)
        , scale(GLMS_VEC3_ONE_INIT)
    {
    }

    mat4s transform_matrix() const;
};

}
