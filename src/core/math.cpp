#include "math.h"

namespace twogame {

mat4s TRS::transform_matrix() const
{
    mat4s t = glms_translate_make(translation),
          r = glms_quat_mat4(rotation),
          s = glms_scale_make(scale);
    return glms_mat4_mul(t, glms_mat4_mul(r, s));
}

}
