#include <physfs.h>
#include <xxhash.h>
#include "asset.h"
#include "render.h"

namespace twogame::asset {

struct Mesh::PrepareData {
    VkBuffer staging, displacements;
    VmaAllocation staging_mem, displacements_mem;
    size_t staging_size, displacements_size;
    std::vector<VkBufferImageCopy> displacements_copies;

    PrepareData(size_t pc)
        : staging(VK_NULL_HANDLE)
        , displacements(VK_NULL_HANDLE)
        , staging_size(0)
        , displacements_size(0)
    {
    }
};

Mesh::Mesh(const xml::assets::Mesh& info, const AssetManager& manager)
    : m_renderer(manager.renderer())
    , m_morph(VK_NULL_HANDLE)
    , m_primitive_groups(info.primitives().size())
    , m_index_buffer_width(VK_INDEX_TYPE_MAX_ENUM)
    , m_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
{
    VkDeviceSize index_buffer_size = 0;
    for (const auto& p : info.primitives()) {
        if (p.indexes())
            index_buffer_size += p.indexes()->range().second;
    }

    m_primitives = std::make_unique<PrimitiveGroup[]>(m_primitive_groups);
    m_prepare_data = std::make_unique<PrepareData>(m_primitive_groups);
    m_prepare_data->staging_size = (index_buffer_size + 15) & ~15;
    m_prepare_data->displacements_size = 0;
    for (const auto& p : info.primitives()) {
        for (const auto& a : p.attributes())
            m_prepare_data->staging_size += (a.range().second + 15) & ~15;
    }
    for (const auto& p : info.primitives()) {
        for (const auto& s : p.displacements())
            m_prepare_data->displacements_size += (s.range().second + 15) & ~15;
    }

    VmaAllocationCreateInfo alloc_ci {};
    VkBufferCreateInfo buffer_ci {};
    VmaAllocationInfo staging_allocinfo, displacements_allocinfo;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = m_prepare_data->staging_size;
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_buffer, &m_buffer_mem, nullptr));
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_prepare_data->staging, &m_prepare_data->staging_mem, &staging_allocinfo));
    if (m_prepare_data->displacements_size) {
        buffer_ci.size = m_prepare_data->displacements_size;
        VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_prepare_data->displacements, &m_prepare_data->displacements_mem, &displacements_allocinfo));
    }

    uint16_t index_buffer_width = 0;
    size_t mapped_offset = 0;
    uint8_t* mapped_buffer = static_cast<uint8_t*>(staging_allocinfo.pMappedData);
    PHYSFS_File* fh = PHYSFS_openRead(info.source().data());
    if (fh == nullptr)
        throw IOException(info.source(), PHYSFS_getLastErrorCode());
    for (size_t p = 0; p < m_primitive_groups; p++) {
        const auto& i = info.primitives().at(p).indexes();
        if (i) {
            if (PHYSFS_seek(fh, i->range().first) == 0)
                throw IOException(info.source(), PHYSFS_getLastErrorCode());
            if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, i->range().second) < static_cast<PHYSFS_sint64>(i->range().second))
                throw IOException(info.source(), PHYSFS_getLastErrorCode());
            if (!i->topology().empty() && !util::parse<>(i->topology(), m_primitive_topology))
                throw MalformedException(info.name(), "bad primitive topology '{}'", i->topology());
            if (index_buffer_width == 0)
                index_buffer_width = i->range().second / i->count();
            else if (index_buffer_width != i->range().second / i->count())
                throw MalformedException(info.name(), "all index buffers must have the same width");
            m_primitives[p].index_count = i->count();
            mapped_offset += i->range().second;
        }
    }
    switch (index_buffer_width) {
    case 0:
        m_index_buffer_width = VK_INDEX_TYPE_NONE_KHR;
        break;
    case 1:
        m_index_buffer_width = VK_INDEX_TYPE_UINT8_EXT;
        break;
    case 2:
        m_index_buffer_width = VK_INDEX_TYPE_UINT16;
        break;
    case 4:
        m_index_buffer_width = VK_INDEX_TYPE_UINT32;
        break;
    default:
        throw MalformedException(info.name(), "invalid index buffer: width={}", index_buffer_width);
    }

    std::vector<std::array<size_t, 3>> pnt_counts(m_primitive_groups);
    mapped_offset = (index_buffer_size + 15) & ~15;
    for (size_t p = 0; p < m_primitive_groups; p++) {
        pnt_counts[p].fill(0);
        m_primitives[p].vertex_count = info.primitives().at(p).count();
        for (const auto& a : info.primitives().at(p).attributes()) {
            if (PHYSFS_seek(fh, a.range().first) == 0)
                throw IOException(info.source(), PHYSFS_getLastErrorCode());
            if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, a.range().second) < static_cast<PHYSFS_sint64>(a.range().second))
                throw IOException(info.source(), PHYSFS_getLastErrorCode());

            size_t ioff = 0;
            if (a.interleaved()) {
                VkVertexInputBindingDescription& d = m_primitives[p].bindings.emplace_back();
                d.binding = m_primitives[p].bindings.size() - 1;
                d.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                d.stride = 0;
                m_primitives[p].binding_offsets.push_back(mapped_offset);
            }
            for (const auto& aa : a.attributes()) {
                VkVertexInputAttributeDescription& d = m_primitives[p].attributes.emplace_back();
                d.location = m_primitives[p].attribute_names.size();
                if (!util::parse<VkFormat>(aa.format(), d.format))
                    throw MalformedException(info.name(), "bad input format {}", aa.format());
                if (a.interleaved()) {
                    d.binding = m_primitives[p].bindings.back().binding;
                    d.offset = m_primitives[p].bindings.back().stride;
                    m_primitives[p].bindings.back().stride += vk::format_width(d.format);
                } else {
                    VkVertexInputBindingDescription& bd = m_primitives[p].bindings.emplace_back();
                    bd.binding = d.binding = m_primitives[p].bindings.size() - 1;
                    bd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                    bd.stride = vk::format_width(d.format);
                    d.offset = 0;
                    m_primitives[p].binding_offsets.push_back(mapped_offset + ioff);
                    ioff += bd.stride * m_primitives[p].vertex_count;
                }

                auto attr_name_it = m_attribute_names.emplace(aa.name());
                m_primitives[p].attribute_names.push_back(*attr_name_it.first);
                if (aa.name() == "position")
                    pnt_counts[p][0] = m_primitives[p].vertex_count;
                else if (aa.name() == "normal")
                    pnt_counts[p][1] = m_primitives[p].vertex_count;
                else if (aa.name() == "tangent")
                    pnt_counts[p][2] = m_primitives[p].vertex_count;
            }
            mapped_offset += (a.range().second + 15) & ~15;
        }
    }

    std::array<int32_t, 3> dacc = { 0, 0, 0 };
    uint32_t morph_channel_count = 0;
    int morph_attrs = -1;
    mapped_offset = 0;
    mapped_buffer = static_cast<uint8_t*>(displacements_allocinfo.pMappedData);
    for (size_t p = 0; p < m_primitive_groups; p++) {
        for (const auto& d : info.primitives().at(p).displacements()) {
            int di;
            if (d.name() == "position")
                di = 0;
            else if (d.name() == "normal")
                di = 1;
            else if (d.name() == "tangent")
                di = 2;
            else
                throw MalformedException(info.name(), "illegal displacements for attribute {}", d.name());

            if (PHYSFS_seek(fh, d.range().first) == 0)
                throw IOException(info.source(), PHYSFS_getLastErrorCode());
            if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, d.range().second) < static_cast<PHYSFS_sint64>(d.range().second))
                throw IOException(info.source(), PHYSFS_getLastErrorCode());

            VkBufferImageCopy& c = m_prepare_data->displacements_copies.emplace_back();
            c.bufferOffset = mapped_offset;
            c.bufferRowLength = c.bufferImageHeight = 0;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.mipLevel = 0;
            c.imageSubresource.baseArrayLayer = di;
            c.imageSubresource.layerCount = 1;
            c.imageOffset = { dacc[di], 0, 0 };
            c.imageExtent.width = pnt_counts[p][di];
            c.imageExtent.height = d.range().second / (pnt_counts[p][di] * 16);
            c.imageExtent.depth = 1;

            morph_attrs = std::max(morph_attrs, di);
            morph_channel_count = std::max(morph_channel_count, c.imageExtent.height);
            mapped_offset += d.range().second;
            dacc[di] += pnt_counts[p][di];
        }
    }

    m_morph = VK_NULL_HANDLE;
    m_morph_position = VK_NULL_HANDLE;
    m_morph_normal = VK_NULL_HANDLE;
    if (m_prepare_data->displacements_copies.size() > 0) {
        VkImageCreateInfo morph_ci {};
        VkImageViewCreateInfo mvci {};
        morph_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        morph_ci.imageType = VK_IMAGE_TYPE_2D;
        morph_ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        morph_ci.extent.width = *std::max_element(dacc.begin(), dacc.end());
        morph_ci.extent.height = morph_channel_count;
        morph_ci.extent.depth = 1;
        morph_ci.mipLevels = 1;
        morph_ci.arrayLayers = morph_attrs + 1;
        morph_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        morph_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        morph_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        morph_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = 0;
        VK_CHECK(vmaCreateImage(m_renderer.allocator(), &morph_ci, &alloc_ci, &m_morph, &m_morph_mem, nullptr));

        mvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        mvci.image = m_morph;
        mvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        mvci.format = morph_ci.format;
        mvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mvci.subresourceRange.baseMipLevel = 0;
        mvci.subresourceRange.levelCount = 1;
        mvci.subresourceRange.layerCount = 1;
        if (morph_ci.arrayLayers > 0) {
            mvci.subresourceRange.baseArrayLayer = 0;
            VK_CHECK(vkCreateImageView(m_renderer.device(), &mvci, nullptr, &m_morph_position));
        }
        if (morph_ci.arrayLayers > 1) {
            mvci.subresourceRange.baseArrayLayer = 1;
            VK_CHECK(vkCreateImageView(m_renderer.device(), &mvci, nullptr, &m_morph_normal));
        }
    }
    m_morph_weights = info.shape_weights();

    XXH3_state_t* xxh = XXH3_createState();
    for (size_t p = 0; p < m_primitive_groups; p++) {
        XXH3_64bits_reset(xxh);

        for (const auto& x : m_primitives[p].attribute_names)
            XXH3_64bits_update(xxh, x.data(), x.size());
        XXH3_64bits_update(xxh, m_primitives[p].attributes.data(), m_primitives[p].attributes.size() * sizeof(VkVertexInputAttributeDescription));
        XXH3_64bits_update(xxh, m_primitives[p].bindings.data(), m_primitives[p].bindings.size() * sizeof(VkVertexInputBindingDescription));
        XXH3_64bits_update(xxh, &m_primitive_topology, sizeof(VkPrimitiveTopology));
        m_primitives[p].pipeline_parameter = XXH3_64bits_digest(xxh);
    }
    XXH3_freeState(xxh);
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_renderer(other.m_renderer)
    , m_buffer(other.m_buffer)
    , m_morph(other.m_morph)
    , m_buffer_mem(other.m_buffer_mem)
    , m_morph_mem(other.m_morph_mem)
    , m_morph_position(other.m_morph_position)
    , m_morph_normal(other.m_morph_normal)
    , m_primitive_groups(other.m_primitive_groups)
    , m_prepare_data(std::move(other.m_prepare_data))
    , m_primitives(std::move(other.m_primitives))
    , m_index_buffer_width(other.m_index_buffer_width)
    , m_primitive_topology(other.m_primitive_topology)
    , m_attribute_names(std::move(other.m_attribute_names))
    , m_morph_weights(std::move(other.m_morph_weights))
{
    other.m_buffer = VK_NULL_HANDLE;
    other.m_morph = VK_NULL_HANDLE;
    other.m_buffer_mem = other.m_morph_mem = VK_NULL_HANDLE;
    other.m_morph_position = other.m_morph_normal = VK_NULL_HANDLE;
}

Mesh::~Mesh()
{
    if (m_morph_position != VK_NULL_HANDLE)
        vkDestroyImageView(m_renderer.device(), m_morph_position, nullptr);
    if (m_morph_normal != VK_NULL_HANDLE)
        vkDestroyImageView(m_renderer.device(), m_morph_normal, nullptr);
    if (m_morph != VK_NULL_HANDLE) {
        vmaDestroyImage(m_renderer.allocator(), m_morph, m_morph_mem);
    }
    vmaDestroyBuffer(m_renderer.allocator(), m_buffer, m_buffer_mem);
}

void Mesh::prepare(VkCommandBuffer cmd)
{
    VkBufferCopy scopy;
    scopy.dstOffset = scopy.srcOffset = 0;
    scopy.size = m_prepare_data->staging_size;
    vkCmdCopyBuffer(cmd, m_prepare_data->staging, m_buffer, 1, &scopy);

    if (m_morph != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_morph;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        if (m_morph_normal != VK_NULL_HANDLE)
            barrier.subresourceRange.layerCount = 2;
        else if (m_morph_position != VK_NULL_HANDLE)
            barrier.subresourceRange.layerCount = 1;
        else
            barrier.subresourceRange.layerCount = 0;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdCopyBufferToImage(cmd, m_prepare_data->displacements, m_morph, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, m_prepare_data->displacements_copies.size(), m_prepare_data->displacements_copies.data());

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

void Mesh::post_prepare()
{
    vmaDestroyBuffer(m_renderer.allocator(), m_prepare_data->staging, m_prepare_data->staging_mem);
    if (m_prepare_data->displacements)
        vmaDestroyBuffer(m_renderer.allocator(), m_prepare_data->displacements, m_prepare_data->displacements_mem);

    m_prepare_data.reset();
}

bool Mesh::prepared() const
{
    return m_prepare_data == nullptr;
}

void Mesh::draw(VkCommandBuffer cmd, uint64_t frame_number, const std::vector<std::shared_ptr<Material>>& materials) const
{
    uint32_t first_index = 0, first_vertex = 0;
    vkCmdBindIndexBuffer(cmd, m_buffer, 0, m_index_buffer_width);

    for (size_t i = 0; i < m_primitive_groups; i++) {
        VkBuffer bound_buffers[m_primitives[i].bindings.size()];
        std::fill(bound_buffers, bound_buffers + m_primitives[i].bindings.size(), m_buffer);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, materials[i]->pipeline(this, i));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, materials[i]->shader()->pipeline_layout(), 3, 1, &materials[i]->descriptor_set(frame_number % 2), 0, nullptr);

        // vkCmdPushConstants(cmd, m_renderer.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &first_vertex);
        vkCmdBindVertexBuffers(cmd, 0, m_primitives[i].bindings.size(), bound_buffers, m_primitives[i].binding_offsets.data());
        vkCmdDrawIndexed(cmd, m_primitives[i].index_count, 1, first_index, 0, 0);
        first_index += m_primitives[i].index_count;
        first_vertex += m_primitives[i].vertex_count;
    }
}

}
