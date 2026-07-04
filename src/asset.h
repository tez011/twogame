#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>
#include <cglm/struct.h>
#include <volk.h>
#include "structs.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IScene;
class SceneManifest;
class StagingBuffer;
namespace fbs {
    struct Assets;
}

class IAsset {
public:
    enum class Type {
        Animation,
        Image,
        Mesh,
        Skeleton,
    };

protected:
    std::variant<std::shared_ptr<void>, uint64_t> m_prepared;

    IAsset() { }

public:
    virtual ~IAsset() { }
    virtual Type type() const = 0;

    virtual size_t prepare_needs() const = 0;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) = 0;
    void post_prepare(uint64_t ready);
};

}

namespace twogame::asset {

class Animation final : public IAsset {
    struct Sampler {
        std::vector<float> timeline;
        std::vector<vec4s> channels;
        uint32_t lerp_targets, step_targets, slerp_targets;

        size_t keyframes() const { return timeline.size(); }
        size_t targets() const { return lerp_targets + step_targets + slerp_targets; }
        void interpolate(float t, vec4* dest, uint32_t* hint, bool force_step) const;
    };

    std::vector<Sampler> m_samplers;
    std::vector<AnimationTarget> m_targets;
    size_t m_keyframe_width;
    float m_duration;

public:
    Animation(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Animation() { }
    inline virtual Type type() const override { return IAsset::Type::Animation; }
    virtual size_t prepare_needs() const override { return 0; }
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override { return 0; }

    float duration() const { return m_duration; }
    float keyframe_width() const { return m_keyframe_width; }
    size_t total_samplers() const { return m_samplers.size(); }
    std::span<const AnimationTarget> targets() const { return m_targets; }

    void interpolate(float t, vec4* dest, std::span<uint32_t> hints, bool force_step = false) const;
};

class Image final : public IAsset {
    VkImage m_image;
    VmaAllocation m_mem;
    VkImageView m_image_view;

public:
    Image(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Image();
    inline virtual Type type() const override { return IAsset::Type::Image; }
    inline VkImage handle() const { return m_image; }
    inline VkImageView view() const { return m_image_view; }

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

class Mesh final : public IAsset {
    size_t m_vertex_count, m_index_count;
    uint32_t m_displacement_count, m_joint_count;

    union {
        uint32_t m_pipeline_key;
        struct {
            unsigned m_uv_channels : 4;
            unsigned m_color_channels : 2;
            unsigned m_32bit_indexes : 1;
        };
    };

public:
    Mesh(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Mesh();
    inline virtual Type type() const override { return IAsset::Type::Mesh; }
    inline size_t index_count() const { return m_index_count; }
    inline uint32_t displacement_count() const { return m_displacement_count; }
    inline uint32_t pipeline_key() const { return m_pipeline_key; }
    size_t write_buffer_addresses(std::span<MeshEntry> mesh_entries, VkDeviceAddress base) const;

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

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
