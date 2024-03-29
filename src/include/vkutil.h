#pragma once
#include <array>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <volk.h>
#include "vk_mem_alloc.h"

namespace twogame {
class Renderer;
}

namespace twogame::vk {

enum class VertexInput {
    Position,
    Normal,
    Joints,
    Weights,
    UV0,
    MAX_VALUE,
};

template <typename T>
bool parse(const std::string_view&, T&);
size_t format_width(VkFormat);
size_t format_width(VkIndexType);

typedef struct {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo details;
} buffer;

class BufferPool {
    const Renderer& m_renderer;
    std::vector<buffer> m_buffers;
    std::vector<bool> m_bits;
    std::vector<bool>::iterator m_bits_it;
    VkDeviceSize m_unit_size, m_count;
    VkBufferUsageFlags m_usage;

    void extend();

public:
    using index_t = uint32_t;
    BufferPool(const Renderer&, VkBufferUsageFlags usage, size_t unit_size, size_t count = 0x4000);
    ~BufferPool();

    index_t allocate();
    void free(index_t);

    VkDeviceSize unit_size() const { return m_unit_size; }
    void buffer_handle(index_t index, VkDescriptorBufferInfo& out) const;
    void* buffer_memory(index_t index, size_t extra_offset = 0) const;
};

class DescriptorPool {
private:
    VkDevice m_device;
    uint32_t m_max_sets;
    std::vector<VkDescriptorPoolSize> m_sizes;

    VkDescriptorSetLayout m_set_layout;
    std::deque<VkDescriptorPool> m_pools, m_pools_full;
    std::deque<VkDescriptorSet> m_free_list;

    void extend();

public:
    DescriptorPool(const Renderer&, const VkDescriptorSetLayoutCreateInfo& layout_info, uint32_t max_sets = 1024);
    ~DescriptorPool();

    inline const VkDescriptorSetLayout& layout() const { return m_set_layout; }

    int allocate(VkDescriptorSet* out, size_t count = 1);
    void free(VkDescriptorSet* sets, size_t count = 1);
    void reset();
};

}
