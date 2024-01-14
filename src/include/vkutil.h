#pragma once
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <volk.h>

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

class DescriptorPool {
private:
    VkDevice m_device;
    uint32_t m_max_sets;
    std::vector<VkDescriptorPoolSize> m_sizes;

    VkDescriptorSetLayout m_set_layout;
    std::deque<VkDescriptorPool> m_pools, m_pools_full;
    std::deque<VkDescriptorSet> m_free_list;

    void create_pool();

public:
    DescriptorPool(const Renderer&, const VkDescriptorSetLayoutCreateInfo& layout_info, uint32_t max_sets = 200);
    ~DescriptorPool();

    inline const VkDescriptorSetLayout& layout() const { return m_set_layout; }

    int allocate(VkDescriptorSet* out, size_t count = 1);
    void free(VkDescriptorSet* sets, size_t count = 1);
    void reset();
};

}
