#pragma once
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace twogame {
class Renderer;
}

namespace twogame::vk {

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

/*
 * Vulkan spec 1.2.267, ss10: VkPipeline is a non-dispatchable handle.
 * Vulkan spec 1.2.267, ss3.3: "Non-dispatchable handle types are a 64-bit
 * integer type whose meaning is implementation-dependent."
 * Therefore, we can use an opaque 64-bit integer type to refer to a pipeline.
 * When PipelineFactory has `VK_EXT_graphics_pipeline_library` enabled, then
 * the PartialPipeline represents a real VkPipeline. When the feature is
 * disabled, the PartialPipeline represents an opaque reference to a field of
 * the PipelineFactory that also describes that partial pipeline state.
 */
typedef uint64_t PartialPipeline;

class PipelineFactory {
private:
    std::map<std::array<PartialPipeline, 2>, VkPipeline> m_pipelines;
    virtual VkPipeline link_pipeline(const std::array<PartialPipeline, 2>&) = 0;

protected:
    const Renderer& m_renderer;

    static void multisample_state_createinfo(const Renderer&, VkPipelineMultisampleStateCreateInfo&);

public:
    PipelineFactory(const Renderer* renderer)
        : m_renderer(*renderer)
    {
    }
    virtual ~PipelineFactory();
    static PipelineFactory* make_pipeline_factory(const Renderer*, bool graphics_pipeline_library_support);

    virtual PartialPipeline vertex_input_state(
        const std::vector<VkVertexInputBindingDescription>& bindings,
        const std::vector<VkVertexInputAttributeDescription>& attributes,
        VkPrimitiveTopology topology)
        = 0;
    virtual PartialPipeline raster_and_fragment_state(
        const std::vector<VkPipelineShaderStageCreateInfo>& shaders,
        VkPipelineLayout pipeline_layout,
        VkRenderPass render_pass,
        uint32_t subpass)
        = 0;
    virtual void destroy_raster_and_fragment_state(PartialPipeline) = 0;

    /**
     * Creates a compute pipeline. This pipeline is owned by the caller.
     */
    VkPipeline compute_pipeline(const VkPipelineShaderStageCreateInfo& stage, VkPipelineLayout layout);

    /**
     * Creates a graphics pipeline. This pipeline is owned by the pipeline factory.
     */
    VkPipeline graphics_pipeline(
        PartialPipeline vertex_input,
        PartialPipeline raster_and_fragment);

    void clear_pipelines(const std::set<VkPipeline>& retain = {});
};

// forward declarations needed so these classes can be friends of renderer
class BasicPipelineFactory;
class GPLPipelineFactory;

}
