#include "render.h"
#include "vkutil.h"

namespace twogame::vk {

DescriptorPool::DescriptorPool(const Renderer& r, const VkDescriptorSetLayoutCreateInfo& layout_info, uint32_t max_sets)
    : m_device(r.device())
    , m_max_sets(max_sets)
{
    m_sizes.reserve(layout_info.bindingCount);
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_set_layout));

    for (uint32_t i = 0; i < layout_info.bindingCount; i++) {
        m_sizes.push_back({ layout_info.pBindings[i].descriptorType, layout_info.pBindings[i].descriptorCount * m_max_sets });
    }

    create_pool();
}

DescriptorPool::~DescriptorPool()
{
    for (VkDescriptorPool& p : m_pools)
        vkDestroyDescriptorPool(m_device, p, nullptr);

    for (VkDescriptorPool& p : m_pools_full)
        vkDestroyDescriptorPool(m_device, p, nullptr);

    vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
}

void DescriptorPool::create_pool()
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
                create_pool();
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
