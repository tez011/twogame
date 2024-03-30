#include <algorithm>
#include <physfs.h>
#include "asset.h"
#include "xml.h"

namespace twogame::asset {

Animation::Animation(const xml::assets::Animation& info, const AssetManager&)
{
    PHYSFS_File* fh = PHYSFS_openRead(info.source().data());
    if (fh == nullptr)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    m_channels.reserve(info.outputs().size());
    m_data = std::make_unique<float[]>(info.range().second);
    m_keyframes = info.keyframes();
    if (info.method() == "step")
        m_interp = InterpolateMethod::Step;
    else if (info.method() == "cubicspline")
        m_interp = InterpolateMethod::CubicSpline;
    else
        m_interp = InterpolateMethod::Linear;

    if (PHYSFS_seek(fh, info.range().first) == 0)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_readBytes(fh, m_data.get(), info.range().second) < static_cast<PHYSFS_sint64>(info.range().second))
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    size_t acc_offset = m_keyframes;
    for (auto it = info.outputs().begin(); it != info.outputs().end(); ++it) {
        Channel& c = m_channels.emplace_back();
        if (it->target() == "translation") {
            c.m_width = 3;
            c.m_target = ChannelTarget::Translation;
        } else if (it->target() == "orientation") {
            acc_offset += (4 - (acc_offset % 4)) % 4; // orientations must exist on a boundary of 4*sizeof(float) == 128 bits, for simd
            c.m_width = 4;
            c.m_target = ChannelTarget::Orientation;
        } else if (it->target() == "shape-weights") {
            c.m_width = it->width();
            c.m_target = ChannelTarget::ShapeWeights;
        } else
            throw MalformedException(info.name(), "bad animation channel target: {}", it->target());

        c.m_data = m_data.get() + acc_offset;
        c.m_bone = it->bone() - 1;
        acc_offset += m_keyframes * c.m_width;
    }
}

bool Animation::is_shapekey() const
{
    return std::any_of(m_channels.begin(), m_channels.end(), [](const Channel& c) { return c.m_target == ChannelTarget::ShapeWeights; });
}

bool Animation::is_skeleton() const
{
    return std::any_of(m_channels.begin(), m_channels.end(), [](const Channel& c) { return c.m_target == ChannelTarget::Translation || c.m_target == ChannelTarget::Orientation; });
}

Animation::Iterator Animation::interpolate(float t) const
{
    return Iterator(this, t);
}

Animation::Iterator::Iterator(const Animation* animation, float t)
    : m_animation(animation)
    , m_it(animation->m_channels.begin())
{
    float *t1 = std::lower_bound(animation->m_data.get(), animation->m_data.get() + animation->m_keyframes - 1, t, std::less_equal {}), *t0 = t1 - 1;
    if (t1 == animation->m_data.get()) {
        m_index = m_iv = 0;
    } else {
        m_index = std::distance(animation->m_data.get(), t0);
        m_iv = (t - *t0) / (*t1 - *t0);
    }
}

Animation::Iterator& Animation::Iterator::operator++()
{
    ++m_it;
    return *this;
}

bool Animation::Iterator::finished() const
{
    return m_it == m_animation->m_channels.end();
}

void Animation::Iterator::get(float* out, size_t count) const
{
    const float *x0 = m_it->m_data + (m_index * m_it->m_width),
                *x1 = m_it->m_data + ((m_index + 1) * m_it->m_width);
    if (m_animation->m_interp == InterpolateMethod::Step) {
        memcpy(out, x0, count * sizeof(float));
    } else if (m_animation->m_interp == InterpolateMethod::Linear) {
        for (size_t i = 0; i < count; i++)
            out[i] = glm_lerp(x0[i], x1[i], m_iv);
    }
}

template <>
void Animation::Iterator::get(vec3s& out) const
{
    const float *x0 = m_it->m_data + (m_index * m_it->m_width),
                *x1 = m_it->m_data + ((m_index + 1) * m_it->m_width);
    if (m_animation->m_interp == InterpolateMethod::Step)
        memcpy(out.raw, x0, sizeof(vec3));
    else if (m_animation->m_interp == InterpolateMethod::Linear)
        glm_vec3_lerp(const_cast<float*>(x0), const_cast<float*>(x1), m_iv, out.raw);
}

template <>
void Animation::Iterator::get(vec4s& out) const
{
    const float *x0 = m_it->m_data + (m_index * m_it->m_width),
                *x1 = m_it->m_data + ((m_index + 1) * m_it->m_width);
    if (m_animation->m_interp == InterpolateMethod::Step)
        memcpy(out.raw, x0, sizeof(vec4));
    else if (m_animation->m_interp == InterpolateMethod::Linear)
        glm_vec4_lerp(const_cast<float*>(x0), const_cast<float*>(x1), m_iv, out.raw);
}

template <>
void Animation::Iterator::get(versors& out) const
{
    const float *x0 = m_it->m_data + (m_index * 4),
                *x1 = m_it->m_data + ((m_index + 1) * 4);
    if (m_animation->m_interp == InterpolateMethod::Step)
        memcpy(out.raw, x0, sizeof(versor));
    else if (m_animation->m_interp == InterpolateMethod::Linear)
        glm_quat_slerp(const_cast<float*>(x0), const_cast<float*>(x1), m_iv, out.raw);
}

Skeleton::Skeleton(const xml::assets::Skeleton& info, const AssetManager&)
{
    PHYSFS_File* fh = PHYSFS_openRead(info.source().data());
    if (fh == nullptr)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_seek(fh, info.range().first) == 0)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    m_inverse_bind_matrices.resize(info.range().second / 64);
    if (PHYSFS_readBytes(fh, m_inverse_bind_matrices.data(), info.range().second) < static_cast<PHYSFS_sint64>(info.range().second))
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    m_default_pose.reserve(m_inverse_bind_matrices.size());
    for (const auto& j : info.joints()) {
        auto& p = m_default_pose.emplace_back();
        p.parent = j.parent();
        p.translation = j.translation();
        p.orientation = j.orientation();
    }
}

Skeleton::Skeleton(Skeleton&& other) noexcept
    : m_inverse_bind_matrices(std::move(other.m_inverse_bind_matrices))
    , m_default_pose(std::move(other.m_default_pose))
{
}

}
