#include "skeleton.h"
#include "asset.fbs.hpp"
#include "core/debug.h"
#include "scene/scene_manifest.h"

namespace twogame::asset {

Skeleton::Skeleton(const SceneManifest& source, size_t source_index, size_t dst_index)
{
    const fbs::Skeleton* info = source.manifest()->skeletons()->Get(source_index);
    m_skin_matrices = source.buffer<mat4s>(info->skin_matrices());

    m_bone_parents.resize(info->nodes()->size(), std::numeric_limits<uint32_t>::max());
    m_bone_transforms.resize(info->nodes()->size(), TRS());
    for (size_t i = 0; i < info->nodes()->size(); i++) {
        const fbs::BoneNode* bnode = info->nodes()->Get(i);
        for (auto it = bnode->children()->begin(); bnode->children() && it != bnode->children()->end(); ++it) {
            SDL_assert(i < *it); // require that parents are ordered before children
            m_bone_parents[*it] = i;
        }

        if (bnode->transform_type() == fbs::Transform::Mat4) {
            mat4s& xfm = m_bone_transforms[i].emplace<mat4s>();
            memcpy(xfm.raw, bnode->transform_as_Mat4(), sizeof(mat4s));
        } else if (bnode->transform_type() == fbs::Transform::TRS) {
            TRS& trs = m_bone_transforms[i].emplace<TRS>();
            memcpy(trs.translation.raw, bnode->transform_as_TRS()->translation().v(), sizeof(vec3));
            memcpy(trs.rotation.raw, bnode->transform_as_TRS()->rotation().v(), sizeof(versor));
            memcpy(trs.scale.raw, bnode->transform_as_TRS()->scale().v(), sizeof(vec3));
        }
    }

    m_joints.reserve(info->joints()->size());
    std::copy(info->joints()->begin(), info->joints()->end(), std::back_inserter(m_joints));
}

}
