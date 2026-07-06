#include "animation.h"
#include <algorithm>
#include "asset.fbs.hpp"
#include "scene/scene_manifest.h"

namespace twogame::asset {

void Animation::Sampler::interpolate(float t, vec4* output, uint32_t* hint, bool force_step) const
{
    uint32_t keyframe = std::numeric_limits<uint32_t>::max(), total_targets = targets();
    if (t < timeline.front()) {
        keyframe = 0;
        force_step = true;
    } else if (t >= timeline.back()) {
        keyframe = static_cast<uint32_t>(timeline.size() - 1);
        force_step = true;
    } else if (hint) {
        uint32_t starting_hint = std::clamp(*hint, 0U, static_cast<uint32_t>(timeline.size() - 1)),
                 max_search = std::min(starting_hint + 6, static_cast<uint32_t>(timeline.size()));
        for (uint32_t hv = starting_hint; hv < max_search; hv++) {
            if (timeline[hv] <= t && (hv + 1 == timeline.size() || t < timeline[hv + 1])) {
                keyframe = hv;
                break;
            }
        }
    }
    if (keyframe == std::numeric_limits<uint32_t>::max()) {
        auto it = std::upper_bound(timeline.begin(), timeline.end(), t);
        if (it == timeline.begin())
            keyframe = 0;
        else
            keyframe = (it - timeline.begin()) - 1;
    }
    if (hint)
        *hint = keyframe;

    float pct = glm_percentc(timeline[keyframe], timeline[keyframe + 1], t);
    if (force_step) {
        memcpy(output, &channels[keyframe * total_targets], total_targets * sizeof(vec4));
    } else {
        memcpy(output, &channels[keyframe * total_targets], step_targets * sizeof(vec4));
        for (uint32_t i = 0; i < lerp_targets; i++)
            glm_vec4_lerp(const_cast<float*>(channels[keyframe * total_targets + step_targets + i].raw),
                const_cast<float*>(channels[(keyframe + 1) * total_targets + step_targets + i].raw),
                pct, reinterpret_cast<float*>(output + step_targets + i));
        for (uint32_t i = 0; i < slerp_targets; i++)
            glm_quat_slerp(const_cast<float*>(channels[keyframe * total_targets + step_targets + lerp_targets + i].raw),
                const_cast<float*>(channels[(keyframe + 1) * total_targets + step_targets + lerp_targets + i].raw),
                pct, reinterpret_cast<float*>(output + step_targets + lerp_targets + i));
    }
}

Animation::Animation(const SceneManifest& source, size_t source_index, size_t dst_index)
    : m_keyframe_width(0)
    , m_duration(0)
{
    const fbs::Animation* info = source.manifest()->animations()->Get(source_index);
    m_samplers.reserve(info->samplers()->size());
    m_targets.reserve(info->targets()->size());
#ifdef DEBUG_BUILD
    size_t keyframe_width_by_sampler = 0;
#endif

    float duration = 0;
    for (auto it = info->samplers()->begin(); it != info->samplers()->end(); ++it) {
        Sampler& sampler = m_samplers.emplace_back();
        sampler.timeline = source.buffer<float>(it->timeline());
        sampler.channels = source.buffer<vec4s>(it->channels());
        sampler.lerp_targets = it->lerp_targets();
        sampler.step_targets = it->step_targets();
        sampler.slerp_targets = it->slerp_targets();
        duration = std::max(duration, sampler.timeline.back());
#ifdef DEBUG_BUILD
        keyframe_width_by_sampler += sampler.lerp_targets + sampler.step_targets + sampler.slerp_targets;
#endif
    }
    for (auto it = info->targets()->begin(); it != info->targets()->end(); ++it) {
        AnimationTarget& target = m_targets.emplace_back();
        target.object = it->object();
        target.width = it->width();
        target.field = static_cast<unsigned>(it->field());
        target.object_is_bone = it->object_is_bone();
        m_keyframe_width += target.width;
    }
#ifdef DEBUG_BUILD
    SDL_assert(keyframe_width_by_sampler == m_keyframe_width);
#endif

    m_duration = ceilf(1000.f * duration);
}

void Animation::interpolate(float t, vec4* dest, std::span<uint32_t> hints, bool force_step) const
{
    size_t tcs = 0;
    for (size_t i = 0; i < m_samplers.size(); i++) {
        m_samplers[i].interpolate(t, dest + tcs, hints.empty() ? nullptr : &hints[i], force_step);
        tcs += m_samplers[i].targets();
    }
}

}
