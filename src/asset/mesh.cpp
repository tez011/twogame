#include <physfs.h>
#include "asset.h"
#include "render.h"
#include "xml.h"

using namespace std::literals;

namespace twogame::asset {

Mesh::Mesh(const xml::assets::Mesh& info, const Renderer* r)
    : AbstractAsset(r)
    , m_buffer_size(0)
{
    for (const auto& attributes : info.attributes()) {
        m_buffer_size += (attributes.range().second + 15) & (~15);
    }
    m_buffer_size += (info.indexes()->range().second + 15) & (~15);

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
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_buffer, &m_buffer_mem, &allocinfo));
    if ((mem_props->memoryTypes[allocinfo.memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        // This buffer is not directly writable; we need a staging buffer too.
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &m_staging, &m_staging_mem, &allocinfo));
    } else {
        m_staging = VK_NULL_HANDLE;
    }

    uint8_t* mapped_buffer = static_cast<uint8_t*>(allocinfo.pMappedData);
    size_t mapped_offset = 0;
    std::string_view last_source;
    PHYSFS_File* fh = nullptr;
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
            VkVertexInputAttributeDescription& a = m_attributes.emplace_back();
            Shader::VertexInput iloc;
            if (vk::parse<VkFormat>(attribute.format(), a.format) == false)
                throw MalformedException(info.name(), "bad shader input format: "s + std::string { attribute.format() });
            if ((iloc = Shader::input_location(attribute.name())) == Shader::VertexInput::MAX_VALUE)
                throw MalformedException(info.name(), "bad shader input location: "s + std::string { attribute.name() });
            else
                a.location = static_cast<uint32_t>(iloc);
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
        }

        mapped_offset += (attributes.range().second + 15) & (~15);
    }

    VkPrimitiveTopology topology;
    m_index_count = info.indexes()->attribute()->count();
    if (!vk::parse<VkIndexType>(info.indexes()->attribute()->format(), m_index_type))
        throw MalformedException(info.name(), "invalid index buffer format "s + std::string { info.indexes()->attribute()->format() });
    if (info.indexes()->topology().empty())
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    else if (!vk::parse<VkPrimitiveTopology>(info.indexes()->topology(), topology))
        throw MalformedException(info.name(), "invalid primitive topology "s + std::string { info.indexes()->topology() });
    m_vertex_input_state = m_renderer.pipeline_factory().vertex_input_state(m_bindings, m_attributes, topology);

    if (last_source != info.indexes()->source()) {
        last_source = info.indexes()->source();
        if (fh)
            PHYSFS_close(fh);
        if ((fh = PHYSFS_openRead(info.indexes()->source().data())) == nullptr)
            throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());
    }
    m_index_offset = mapped_offset;
    if (PHYSFS_seek(fh, info.indexes()->range().first) == 0)
        throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());
    if (PHYSFS_readBytes(fh, mapped_buffer + mapped_offset, info.indexes()->range().second) < static_cast<PHYSFS_sint64>(info.indexes()->range().second))
        throw IOException(info.indexes()->source(), PHYSFS_getLastErrorCode());

    if (fh)
        PHYSFS_close(fh);
}

Mesh::Mesh(Mesh&& other) noexcept
    : AbstractAsset(&other.m_renderer)
    , m_buffer(other.m_buffer)
    , m_staging(other.m_staging)
    , m_buffer_mem(other.m_buffer_mem)
    , m_staging_mem(other.m_staging_mem)
    , m_buffer_size(other.m_buffer_size)
    , m_attributes(std::move(other.m_attributes))
    , m_bindings(std::move(other.m_bindings))
    , m_binding_offsets(std::move(other.m_binding_offsets))
    , m_index_offset(other.m_index_offset)
    , m_index_count(other.m_index_count)
    , m_index_type(other.m_index_type)
    , m_vertex_input_state(other.m_vertex_input_state)
{
    other.m_buffer = VK_NULL_HANDLE;
    other.m_staging = VK_NULL_HANDLE;
    other.m_vertex_input_state = 0;
}

Mesh::~Mesh()
{
    if (m_staging != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_renderer.allocator(), m_staging, m_staging_mem);
    vmaDestroyBuffer(m_renderer.allocator(), m_buffer, m_buffer_mem);
}

void Mesh::bind_buffers(VkCommandBuffer cmd)
{
    VkBuffer bound_buffers[m_bindings.size()];
    std::fill(bound_buffers, bound_buffers + m_bindings.size(), m_buffer);

    vkCmdBindVertexBuffers(cmd, 0, m_bindings.size(), bound_buffers, m_binding_offsets.data());
    vkCmdBindIndexBuffer(cmd, m_buffer, m_index_offset, m_index_type);
}

void Mesh::prepare(VkCommandBuffer cmd)
{
    if (m_staging != VK_NULL_HANDLE) {
        VkBufferCopy region {};
        region.srcOffset = region.dstOffset = 0;
        region.size = m_buffer_size;
        vkCmdCopyBuffer(cmd, m_staging, m_buffer, 1, &region);
    }
}

void Mesh::post_prepare()
{
    if (m_staging != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_renderer.allocator(), m_staging, m_staging_mem);
        m_staging = VK_NULL_HANDLE;
    }
}

bool Mesh::prepared() const
{
    return m_staging == VK_NULL_HANDLE;
}

}
