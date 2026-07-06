#pragma once
#include <span>
#include <vector>
#include "assets/asset.h"
#include "core/math.h"

namespace twogame::asset {

class Skeleton final : public IAsset {
    std::vector<uint32_t> m_bone_parents;
    std::vector<std::variant<mat4s, TRS>> m_bone_transforms;
    std::vector<uint32_t> m_joints;
    std::vector<mat4s> m_skin_matrices;

public:
    Skeleton(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Skeleton() { }
    inline virtual Type type() const override { return IAsset::Type::Skeleton; }
    inline std::span<const uint32_t> bone_parents() const { return m_bone_parents; }
    inline std::span<const std::variant<mat4s, TRS>> bone_transforms() const { return m_bone_transforms; }
    inline std::span<const uint32_t> joints() const { return m_joints; }
    inline std::span<const mat4s> skin_matrices() const { return m_skin_matrices; }

    virtual size_t prepare_needs() const override { return 0; }
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override { return 0; }
};

}
