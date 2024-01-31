#include <algorithm>
#include "render.h"
#include "vkutil.h"

namespace twogame::vk {

BufferPool::BufferPool(const Renderer& r, VkBufferUsageFlags usage, size_t unit_size, size_t count)
    : m_renderer(r)
    , m_count(count)
    , m_usage(usage)
{
    VkDeviceSize alignment = 1; // this is guaranteed per spec to be a power of two
    if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        alignment = std::max(alignment, r.limits().minUniformBufferOffsetAlignment);
    if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
        alignment = std::max(alignment, r.limits().minStorageBufferOffsetAlignment);
    m_unit_size = (unit_size + (alignment - 1)) & ~(alignment - 1);
    extend();
}

BufferPool::~BufferPool()
{
    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
        vmaDestroyBuffer(m_renderer.allocator(), it->buffer, it->allocation);
    }
}

void BufferPool::extend()
{
    VkBufferCreateInfo buffer_ci {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = m_unit_size * m_count;
    buffer_ci.usage = m_usage;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci {};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    auto& out = m_buffers.emplace_back();
    VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &out.buffer, &out.allocation, &out.details));
    m_bits.resize(m_bits.size() + m_count);
    m_bits_it = m_bits.end() - m_count;
}

BufferPool::index_t BufferPool::allocate()
{
    if (m_bits_it == m_bits.end())
        m_bits_it = m_bits.begin();
    if (*m_bits_it) {
        m_bits_it = std::find(m_bits_it, m_bits.end(), false);
        if (m_bits_it == m_bits.end()) {
            extend();
        }
    }
    return static_cast<BufferPool::index_t>(std::distance(m_bits.begin(), m_bits_it++));
}

void BufferPool::free(BufferPool::index_t i)
{
    m_bits[i] = false;
    m_bits_it = m_bits.begin() + i;
}

void BufferPool::buffer_handle(BufferPool::index_t index, VkDescriptorBufferInfo& out) const
{
    out.buffer = m_buffers[index / m_count].buffer;
    out.offset = m_unit_size * (index % m_count);
    out.range = m_unit_size;
}

void* BufferPool::buffer_memory(BufferPool::index_t index, size_t extra_offset) const
{
    auto address = reinterpret_cast<uintptr_t>(m_buffers[index / m_count].details.pMappedData);
    return reinterpret_cast<void*>(address + (m_unit_size * (index % m_count)) + extra_offset);
}

void BufferPool::flush(BufferPool::index_t* indexes, size_t count) const
{
    std::vector<VkMappedMemoryRange> flushes(count);
    for (size_t i = 0; i < count; i++) {
        flushes[i].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flushes[i].pNext = nullptr;
        flushes[i].memory = m_buffers[i / m_count].details.deviceMemory;
        flushes[i].offset = m_unit_size * (i % m_count);
        flushes[i].size = std::max(m_renderer.limits().nonCoherentAtomSize, m_unit_size);
    }
    vkFlushMappedMemoryRanges(m_renderer.device(), count, flushes.data());
}

DescriptorPool::DescriptorPool(const Renderer& r, const VkDescriptorSetLayoutCreateInfo& layout_info, uint32_t max_sets)
    : m_device(r.device())
    , m_max_sets(max_sets)
{
    m_sizes.reserve(layout_info.bindingCount);
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_set_layout));

    for (uint32_t i = 0; i < layout_info.bindingCount; i++) {
        m_sizes.push_back({ layout_info.pBindings[i].descriptorType, layout_info.pBindings[i].descriptorCount * m_max_sets });
    }

    extend();
}

DescriptorPool::~DescriptorPool()
{
    for (VkDescriptorPool& p : m_pools)
        vkDestroyDescriptorPool(m_device, p, nullptr);

    for (VkDescriptorPool& p : m_pools_full)
        vkDestroyDescriptorPool(m_device, p, nullptr);

    vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
}

void DescriptorPool::extend()
{
    VkDescriptorPoolCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = m_max_sets;
    ci.poolSizeCount = m_sizes.size();
    ci.pPoolSizes = m_sizes.data();

    VK_CHECK(vkCreateDescriptorPool(m_device, &ci, nullptr, &m_pools.emplace_back()));
}

int DescriptorPool::allocate(VkDescriptorSet* out, size_t count)
{
    if (m_free_list.empty()) {
        VkDescriptorSetLayout layouts[count];
        std::fill(layouts, layouts + count, m_set_layout);

        VkDescriptorSetAllocateInfo allocinfo {};
        VkResult res;
        allocinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocinfo.descriptorPool = m_pools.front();
        allocinfo.descriptorSetCount = count;
        allocinfo.pSetLayouts = layouts;
        if ((res = vkAllocateDescriptorSets(m_device, &allocinfo, out)) == VK_SUCCESS) {
            return count;
        } else if (res == VK_ERROR_FRAGMENTED_POOL || res == VK_ERROR_OUT_OF_POOL_MEMORY) {
            m_pools_full.push_back(m_pools.front());
            m_pools.pop_front();
            if (m_pools.empty())
                extend();
            return allocate(out, count); // try again with a new pool
        } else {
            VK_CHECK(res);
            return 0;
        }
    } else {
        size_t ec = std::min(count, m_free_list.size());
        std::copy(m_free_list.begin(), m_free_list.begin() + ec, out);
        m_free_list.erase(m_free_list.begin(), m_free_list.begin() + ec);
        if (ec < count)
            return ec + allocate(out + ec, count - ec);
        else
            return ec;
    }
}

void DescriptorPool::free(VkDescriptorSet* sets, size_t count)
{
    m_free_list.insert(m_free_list.end(), sets, sets + count);
}

void DescriptorPool::reset()
{
    for (VkDescriptorPool& p : m_pools)
        vkResetDescriptorPool(m_device, p, 0);
    for (VkDescriptorPool& p : m_pools_full)
        vkResetDescriptorPool(m_device, p, 0);

    m_pools.insert(m_pools.end(), m_pools_full.begin(), m_pools_full.end());
    m_pools_full.clear();
}

}
