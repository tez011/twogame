#pragma once
#include <span>
#include <vector>
#include <cglm/struct.h>
#include "asset.h"

namespace twogame {

struct AnimationTarget {
    struct Field {
        enum {
            Translation = 1,
            Rotation,
            Scale,
            Weights,
        };
    };
    uint32_t object;
    uint16_t width;
    union {
        uint16_t _uv;
        struct {
            unsigned field : 4;
            unsigned object_is_bone : 1;
        };
    };
};

struct AnimationInstance {
    uint64_t start_time;
    uint32_t animation_index;
    std::unique_ptr<uint32_t[]> keyframe_hints;
    std::variant<std::monostate, // Use the node target from the animation asset
        std::unique_ptr<uint32_t[]>, // We specify our own node target
        std::weak_ptr<uint32_t[]> // This is a skeletal animation, and these are the skin nodes
        >
        custom_targets;
    bool loop;
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
    ssize_t m_duration;

public:
    Animation(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Animation() { }
    inline virtual Type type() const override { return IAsset::Type::Animation; }
    virtual size_t prepare_needs() const override { return 0; }
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override { return 0; }

    ssize_t duration() const { return m_duration; }
    size_t keyframe_width() const { return m_keyframe_width; }
    size_t total_samplers() const { return m_samplers.size(); }
    std::span<const AnimationTarget> targets() const { return m_targets; }

    void interpolate(float t, vec4* dest, std::span<uint32_t> hints, bool force_step = false) const;
};

}
