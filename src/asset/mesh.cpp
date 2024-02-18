#include <physfs.h>
#include "asset.h"
#include "render.h"
#include "xml.h"
#include "xxhash.h"

using namespace std::literals;

namespace twogame::asset {

Mesh::Mesh(const xml::assets::Mesh& info, const Renderer* r)
    : m_renderer(*r)
    , m_displacement_prep({})
    , m_index_count(info.indexes()->count())
{
    if (!vk::parse<VkIndexType>(info.indexes()->format(), m_index_type))
        throw MalformedException(info.name(), "invalid index buffer format "s + std::string { info.indexes()->format() });

    VkDeviceSize index_buffer_size = info.indexes()->count() * vk::format_width(m_index_type);
    m_buffer_size = (index_buffer_size + 15) & (~15);
    for (const auto& attributes : info.attributes())
        m_buffer_size += (attributes.range().second + 15) & (~15);
    if (info.displacements())
        m_buffer_size += (info.displacements()->range().second + 15) & (~15);

    const VkPhysicalDeviceMemoryProperties* mem_props;
    vmaGetMemoryProperties(m_renderer.allocator(), &mem_props);

    VmaAllocationCreateInfo alloc_ci {};
    VkBufferCreateInfo buffer_ci {};
    VmaAllocationInfo allocinfo;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = m_buffer_size;
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_buffer, &m_buffer_mem, &allocinfo));
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_staging, &m_staging_mem, &allocinfo));

    uint8_t* mapped_buffer = static_cast<uint8_t*>(allocinfo.pMappedData);
    std::string_view last_source = info.indexes()->source();
    PHYSFS_File* fh = PHYSFS_openRead(info.indexes()->source().data());
    if (fh == nullptr)
        throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_seek(fh, info.indexes()->offset()) == 0)
        throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_readBytes(fh, mapped_buffer + 0, index_buffer_size) < static_cast<PHYSFS_sint64>(index_buffer_size))
        throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());

    std::bitset<static_cast<size_t>(vk::VertexInput::MAX_VALUE)> vertex_inputs;
    size_t mapped_offset = (index_buffer_size + 15) & (~15);
    for (const auto& attributes : info.attributes()) {
        if (last_source != attributes.source()) {
            last_source = attributes.source();
            if (fh)
                PHYSFS_close(fh);
            if ((fh = PHYSFS_openRead(attributes.source().data())) == nullptr)
                throw IOException(attributes.source(), PHYSFS_getLastErrorCode());
        }
        if (PHYSFS_seek(fh, attributes.range().first) == 0)
            throw IOException(attributes.source(), PHYSFS_getLastErrorCode());
        if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, attributes.range().second) < static_cast<PHYSFS_sint64>(attributes.range().second))
            throw IOException(attributes.source(), PHYSFS_getLastErrorCode());

        size_t internal_offset = 0;
        if (attributes.interleaved()) {
            VkVertexInputBindingDescription& b = m_bindings.emplace_back();
            b.binding = m_bindings.size() - 1;
            b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            b.stride = 0;
            m_binding_offsets.push_back(mapped_offset);
        }
        for (const auto& attribute : attributes.attributes()) {
            VertexInputAttribute& a = m_attributes.emplace_back();
            if (vk::parse<VkFormat>(attribute.format(), a.format) == false)
                throw MalformedException(info.name(), "bad shader input format: "s + std::string { attribute.format() });
            if (vk::parse<vk::VertexInput>(attribute.name(), a.field) == false)
                throw MalformedException(info.name(), "bad shader input location: "s + std::string { attribute.name() });
            if (attributes.interleaved()) {
                a.binding = m_bindings.back().binding;
                a.offset = m_bindings.back().stride;
                m_bindings.back().stride = vk::format_width(a.format);
            } else {
                VkVertexInputBindingDescription& b = m_bindings.emplace_back();
                b.binding = m_bindings.size() - 1;
                b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                b.stride = vk::format_width(a.format);
                a.binding = b.binding;
                a.offset = 0;
                m_binding_offsets.push_back(mapped_offset + internal_offset);
                internal_offset += b.stride * attribute.count();
            }
            vertex_inputs.set(static_cast<size_t>(a.field), true);
        }

        mapped_offset += (attributes.range().second + 15) & (~15);
    }
    if (info.displacements()) {
        if (last_source != info.displacements()->source()) {
            last_source = info.displacements()->source();
            if (fh)
                PHYSFS_close(fh);
            if ((fh = PHYSFS_openRead(info.displacements()->source().data())) == nullptr)
                throw IOException(info.displacements()->source(), PHYSFS_getLastErrorCode());
        }
        m_displacement_prep.bufferOffset = mapped_offset;
        if (PHYSFS_seek(fh, info.displacements()->range().first) == 0)
            throw IOException(info.displacements()->source(), PHYSFS_getLastErrorCode());
        if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, info.displacements()->range().second) < static_cast<PHYSFS_sint64>(info.displacements()->range().second))
            throw IOException(info.displacements()->source(), PHYSFS_getLastErrorCode());
        mapped_offset += (info.displacements()->range().second + 15) & (~15);

        size_t morph_target_count = info.displacements()->displacements().size(),
               morph_sampler_height = morph_target_count * (vertex_inputs.test(static_cast<size_t>(vk::VertexInput::Normal)) ? 2 : 1);
        m_displacement_initial_weights.reserve(morph_target_count);
        for (auto it = info.displacements()->displacements().begin(); it != info.displacements()->displacements().end(); ++it)
            m_displacement_initial_weights.emplace_back(it->weight());

        VkImageCreateInfo dici {};
        VmaAllocationCreateInfo diai {};
        dici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dici.imageType = VK_IMAGE_TYPE_1D;
        dici.format = VK_FORMAT_R32G32B32A32_SFLOAT; // displacements are vec4's, since hardware support is required for this
        dici.extent.width = info.displacements()->range().second / (morph_sampler_height * 16);
        dici.extent.height = dici.extent.depth = 1;
        dici.mipLevels = 1;
        dici.arrayLayers = morph_sampler_height;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_OPTIMAL;
        dici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        dici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        diai.usage = VMA_MEMORY_USAGE_AUTO;
        m_displacement_prep.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        m_displacement_prep.imageSubresource.mipLevel = 0;
        m_displacement_prep.imageSubresource.baseArrayLayer = 0;
        m_displacement_prep.imageSubresource.layerCount = dici.arrayLayers;
        m_displacement_prep.imageExtent = dici.extent;
        VK_CHECK(vmaCreateImage(m_renderer.allocator(), &dici, &diai, &m_displacement_image, &m_displacement_mem, nullptr));

        VkImageViewCreateInfo dvci {};
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvci.image = m_displacement_image;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        dvci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        dvci.subresourceRange.baseMipLevel = 0;
        dvci.subresourceRange.levelCount = 1;
        dvci.subresourceRange.baseArrayLayer = 0;
        dvci.subresourceRange.layerCount = morph_target_count;
        VK_CHECK(vkCreateImageView(m_renderer.device(), &dvci, nullptr, &m_displacement_position));
        if (vertex_inputs.test(static_cast<size_t>(vk::VertexInput::Normal))) {
            dvci.subresourceRange.baseArrayLayer = dvci.subresourceRange.layerCount = morph_target_count;
            VK_CHECK(vkCreateImageView(m_renderer.device(), &dvci, nullptr, &m_displacement_normal));
        } else {
            m_displacement_normal = VK_NULL_HANDLE;
        }
    } else {
        m_displacement_image = VK_NULL_HANDLE;
        m_displacement_position = m_displacement_normal = VK_NULL_HANDLE;
    }
    if (fh)
        PHYSFS_close(fh);

    m_primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (!info.indexes()->topology().empty() && !vk::parse<VkPrimitiveTopology>(info.indexes()->topology(), m_primitive_topology))
        throw MalformedException(info.name(), "invalid primitive topology "s + std::string { info.indexes()->topology() });
    for (auto it = info.animations().begin(); it != info.animations().end(); ++it)
        m_animations[std::string { it->name() }] = std::make_shared<Animation>(*it, this);

    XXH3_state_t* xxh = XXH3_createState();
    XXH3_64bits_reset(xxh);
    XXH3_64bits_update(xxh, m_attributes.data(), m_attributes.size() * sizeof(decltype(m_attributes)::value_type));
    XXH3_64bits_update(xxh, m_bindings.data(), m_bindings.size() * sizeof(decltype(m_bindings)::value_type));
    XXH3_64bits_update(xxh, &m_primitive_topology, sizeof(m_primitive_topology));
    m_pipeline_parameter = XXH3_64bits_digest(xxh);
    XXH3_freeState(xxh);
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_renderer(other.m_renderer)
    , m_buffer(other.m_buffer)
    , m_staging(other.m_staging)
    , m_buffer_mem(other.m_buffer_mem)
    , m_staging_mem(other.m_staging_mem)
    , m_buffer_size(other.m_buffer_size)
    , m_displacement_image(other.m_displacement_image)
    , m_displacement_position(other.m_displacement_position)
    , m_displacement_normal(other.m_displacement_normal)
    , m_displacement_mem(other.m_displacement_mem)
    , m_displacement_prep(other.m_displacement_prep)
    , m_attributes(std::move(other.m_attributes))
    , m_bindings(std::move(other.m_bindings))
    , m_binding_offsets(std::move(other.m_binding_offsets))
    , m_primitive_topology(other.m_primitive_topology)
    , m_index_count(other.m_index_count)
    , m_index_type(other.m_index_type)
{
    other.m_buffer = VK_NULL_HANDLE;
    other.m_staging = VK_NULL_HANDLE;
    other.m_displacement_image = VK_NULL_HANDLE;
}

Mesh::~Mesh()
{
    if (m_staging != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_renderer.allocator(), m_staging, m_staging_mem);
    if (m_displacement_image != VK_NULL_HANDLE) {
        vkDestroyImageView(m_renderer.device(), m_displacement_normal, nullptr);
        vkDestroyImageView(m_renderer.device(), m_displacement_position, nullptr);
        vmaDestroyImage(m_renderer.allocator(), m_displacement_image, m_displacement_mem);
    }
    vmaDestroyBuffer(m_renderer.allocator(), m_buffer, m_buffer_mem);
}

void Mesh::bind_buffers(VkCommandBuffer cmd)
{
    VkBuffer bound_buffers[m_bindings.size()];
    std::fill(bound_buffers, bound_buffers + m_bindings.size(), m_buffer);

    vkCmdBindVertexBuffers(cmd, 0, m_bindings.size(), bound_buffers, m_binding_offsets.data());
    vkCmdBindIndexBuffer(cmd, m_buffer, 0, m_index_type);
}

void Mesh::prepare(VkCommandBuffer cmd)
{
    VkBufferCopy region {};
    region.srcOffset = region.dstOffset = 0;
    region.size = m_buffer_size;
    vkCmdCopyBuffer(cmd, m_staging, m_buffer, 1, &region);
    if (m_displacement_image != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_displacement_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = m_displacement_prep.imageSubresource.layerCount;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdCopyBufferToImage(cmd, m_staging, m_displacement_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &m_displacement_prep);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

void Mesh::post_prepare()
{
    vmaDestroyBuffer(m_renderer.allocator(), m_staging, m_staging_mem);
    m_staging = VK_NULL_HANDLE;
    if (m_displacement_image != VK_NULL_HANDLE) {
        m_displacement_prep.imageExtent.width = 0;
    }
}

bool Mesh::prepared() const
{
    if (m_displacement_image != VK_NULL_HANDLE && m_displacement_prep.imageExtent.width != 0)
        return false;
    return m_staging == VK_NULL_HANDLE;
}

Animation::Animation(const xml::assets::Animation& info, Mesh* mesh)
{
    PHYSFS_File* fh = PHYSFS_openRead(info.source().data());
    if (fh == nullptr)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    m_inputs.resize(info.keyframes());
    m_channels.reserve(info.outputs().size());
    if (PHYSFS_seek(fh, info.input_offset()) == 0)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_readBytes(fh, m_inputs.data(), m_inputs.size() * sizeof(float)) < static_cast<PHYSFS_sint64>(m_inputs.size() * sizeof(float)))
        throw IOException(info.source(), PHYSFS_getLastErrorCode());

    for (auto it = info.outputs().begin(); it != info.outputs().end(); ++it) {
        Channel& c = m_channels.emplace_back();
        c.m_bone = it->bone();
        c.m_step_interpolate = it->step_interpolate();
        if (it->target() == "translation") {
            c.m_target = ChannelTarget::Translation;
            c.m_data.resize(info.keyframes() * 3);
        } else if (it->target() == "orientation") {
            c.m_target = ChannelTarget::Orientation;
            c.m_data.resize(info.keyframes() * 4);
        } else if (it->target() == "displacements") {
            if (mesh == nullptr)
                throw MalformedException(info.name(), "animation with displacement weights must be associated with a mesh");
            c.m_target = ChannelTarget::DisplaceWeights;
            c.m_data.resize(info.keyframes() * mesh->displacement_initial_weights().size());
        }
        if (PHYSFS_seek(fh, it->offset()) == 0)
            throw IOException(info.source(), PHYSFS_getLastErrorCode());
        if (PHYSFS_readBytes(fh, c.m_data.data(), c.m_data.size() * sizeof(float)) < static_cast<PHYSFS_sint64>(c.m_data.size() * sizeof(float)))
            throw IOException(info.source(), PHYSFS_getLastErrorCode());
    }
}

Animation::Iterator Animation::interpolate(float t) const
{
    return Iterator(this, t);
}

bool Animation::finished(const Iterator& it) const
{
    return it.m_it == m_channels.end();
}

Animation::Iterator::Iterator(const Animation* animation, float t)
    : m_animation(animation)
{
    size_t begin = 0, end = animation->m_inputs.size() - 1, res = -1;
    while (begin <= end) {
        size_t mid = (begin + end) / 2;
        if (animation->m_inputs[mid] <= t) {
            res = mid;
            begin = mid + 1;
        } else {
            end = mid - 1;
        }
    }

    float t0 = animation->m_inputs[res], t1 = animation->m_inputs[res + 1];
    m_it = animation->m_channels.begin();
    m_index = res;
    m_iv = (t - t0) / (t1 - t0);
}

Animation::Iterator& Animation::Iterator::operator++()
{
    ++m_it;
    return *this;
}

void Animation::Iterator::get(size_t count, float* out) const
{
    size_t dim;
    switch (m_it->m_target) {
    case ChannelTarget::Translation:
        dim = 3;
        break;
    case ChannelTarget::Orientation:
        dim = 4;
        break;
    default:
        dim = m_it->m_data.size() / m_animation->m_inputs.size();
        break;
    }
#ifdef TWOGAME_DEBUG_BUILD
    if (count < dim)
        spdlog::error("buffer provided to Animation::Iterator::get was {} big; needed {}", count, dim);
#endif

    const float *x0 = m_it->m_data.data() + (m_index * dim),
                *x1 = m_it->m_data.data() + ((m_index + 1) * dim);
    if (m_it->m_step_interpolate) {
        memcpy(out, x0, std::min(count, dim) * sizeof(float));
    } else if (m_it->m_target == ChannelTarget::Orientation) {
        glm_quat_slerp(const_cast<float*>(x0), const_cast<float*>(x1), m_iv, out);
    } else {
        for (size_t i = 0; i < std::min(count, dim); i++)
            out[i] = glm_lerp(x0[i], x1[i], m_iv);
    }
}

}
