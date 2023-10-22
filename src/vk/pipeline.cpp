#include <map>
#include <memory>
#include "render.h"
#include "xxh64.hpp"

static void vertex_input_state_createinfos(
    const std::vector<VkVertexInputBindingDescription>& bindings,
    const std::vector<VkVertexInputAttributeDescription>& attributes,
    VkPrimitiveTopology topology,
    VkPipelineVertexInputStateCreateInfo& vertex_input_state,
    VkPipelineInputAssemblyStateCreateInfo& input_assembly_state)
{
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.vertexBindingDescriptionCount = bindings.size();
    vertex_input_state.pVertexBindingDescriptions = bindings.data();
    vertex_input_state.vertexAttributeDescriptionCount = attributes.size();
    vertex_input_state.pVertexAttributeDescriptions = attributes.data();
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.topology = topology;
}

static VkViewport viewport_state_viewport {};
static VkRect2D viewport_state_scissor {};
static VkPipelineViewportStateCreateInfo viewport_state = {
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    nullptr,
    0,
    1,
    &viewport_state_viewport,
    1,
    &viewport_state_scissor,
};

static VkPipelineRasterizationStateCreateInfo rasterization_state = {
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    nullptr,
    0,
    VK_FALSE,
    VK_FALSE,
    VK_POLYGON_MODE_FILL,
    VK_CULL_MODE_BACK_BIT,
    VK_FRONT_FACE_COUNTER_CLOCKWISE,
    VK_FALSE, // depthBiasEnable
    0.f, // depthBiasConstantFactor
    0.f, // depthBiasClamp
    0.f, // depthBiasSlopeFactor
    1.f,
};

static VkPipelineTessellationStateCreateInfo tessellation_state = {
    VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
    nullptr,
    0,
    3, // patchControlPoints
};

static VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    nullptr,
    0,
    VK_TRUE, // depthTestEnable
    VK_TRUE, // depthWriteEnable
    VK_COMPARE_OP_LESS, // depthCompareOp
    VK_FALSE, // depthBoundsTestEnable
    VK_FALSE, // stencilTestEnable,
    {}, // front (VkStencilOpState)
    {}, // back (VkStencilOpState)
    0.f, // minDepthBounds
    0.f, // maxDepthBounds
};

static VkPipelineColorBlendAttachmentState color_blend_attachments[] = {
    {
        VK_FALSE, // blendEnable
        VK_BLEND_FACTOR_SRC_ALPHA, // srcColorBlendFactor
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // dstColorBlendFactor
        VK_BLEND_OP_ADD, // colorBlendOp
        VK_BLEND_FACTOR_ONE, // srcAlphaBlendFactor
        VK_BLEND_FACTOR_ZERO, // dstAlphaBlendFactor
        VK_BLEND_OP_ADD, // alphaBlendOp
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    },
};

static VkPipelineColorBlendStateCreateInfo color_blend_state = {
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    nullptr,
    0,
    VK_FALSE, // logicOpEnable
    VK_LOGIC_OP_CLEAR, // logicOp
    1, // attachmentCount
    color_blend_attachments + 0, // pAttachments
    { 0.f, 0.f, 0.f, 0.f }, // blendConstants
};

static VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
static VkPipelineDynamicStateCreateInfo dynamic_state {
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    nullptr,
    0,
    2,
    dynamic_states,
};

namespace twogame::vk {

void PipelineFactory::multisample_state_createinfo(const Renderer& r, VkPipelineMultisampleStateCreateInfo& out)
{
    out.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    out.rasterizationSamples = static_cast<VkSampleCountFlagBits>(r.m_multisample_count),
    out.sampleShadingEnable = r.m_sample_shading > 0 ? VK_TRUE : VK_FALSE;
    out.minSampleShading = r.m_sample_shading;
    out.pSampleMask = nullptr;
    out.alphaToCoverageEnable = VK_FALSE;
    out.alphaToOneEnable = VK_FALSE;
}

class BasicPipelineFactory final : public twogame::vk::PipelineFactory {
    using vertex_input_rev_key_t = std::tuple<size_t, size_t, VkPrimitiveTopology>;
    using raster_fragment_rev_key_t = std::tuple<VkPipelineLayout, VkRenderPass, uint32_t>;
    typedef struct {
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        VkPrimitiveTopology topology;
    } vertex_input_state_t;

    uint64_t m_fake_pipeline_counter = 0x1001;
    std::map<PartialPipeline, vertex_input_state_t> m_vertex_input_states;
    std::map<vertex_input_rev_key_t, std::deque<PartialPipeline>> m_rev_vertex_input_states;
    std::map<PartialPipeline, raster_fragment_rev_key_t> m_raster_fragment_states;
    std::map<PartialPipeline, std::pair<size_t, const VkPipelineShaderStageCreateInfo*>> m_rf_shader_states;
    std::map<raster_fragment_rev_key_t, PartialPipeline> m_rev_raster_fragment_states;

public:
    BasicPipelineFactory(const Renderer* r)
        : PipelineFactory(r)
    {
    }

    virtual ~BasicPipelineFactory()
    {
    }

    virtual PartialPipeline vertex_input_state(
        const std::vector<VkVertexInputBindingDescription>& bindings,
        const std::vector<VkVertexInputAttributeDescription>& attributes,
        VkPrimitiveTopology topology)
    {
        vertex_input_rev_key_t key = { bindings.size(), attributes.size(), topology };
        for (const auto& candidate : m_rev_vertex_input_states[key]) {
            const auto& info = m_vertex_input_states[candidate];
            if (memcmp(bindings.data(), info.bindings.data(), sizeof(VkVertexInputBindingDescription) * bindings.size()) != 0)
                continue;
            if (memcmp(attributes.data(), info.attributes.data(), sizeof(VkVertexInputAttributeDescription) * attributes.size()) != 0)
                continue;
            return candidate;
        }

        PartialPipeline np = m_fake_pipeline_counter++;
        m_vertex_input_states[np].bindings = bindings; // copy
        m_vertex_input_states[np].attributes = attributes;
        m_vertex_input_states[np].topology = topology;
        m_rev_vertex_input_states[key].push_back(np);
        return np;
    }

    virtual PartialPipeline raster_and_fragment_state(
        const std::vector<VkPipelineShaderStageCreateInfo>& shaders,
        VkPipelineLayout pipeline_layout,
        VkRenderPass render_pass,
        uint32_t subpass)
    {
        raster_fragment_rev_key_t key = { pipeline_layout, render_pass, subpass };
        if (m_rev_raster_fragment_states[key] == 0) {
            PartialPipeline np = m_fake_pipeline_counter++;
            m_raster_fragment_states[np] = key;
            m_rf_shader_states[np] = { shaders.size(), shaders.data() };
            m_rev_raster_fragment_states[key] = np;
            return np;
        } else {
            return m_rev_raster_fragment_states[key];
        }
    }

    virtual void destroy_raster_and_fragment_state(PartialPipeline key)
    {
        raster_fragment_rev_key_t skey = m_raster_fragment_states[key];
        m_rf_shader_states.erase(key);
        m_rev_raster_fragment_states[skey] = 0;
    }

    virtual VkPipeline link_pipeline(const std::array<PartialPipeline, 2>& pp)
    {
        VkGraphicsPipelineCreateInfo createinfo {};
        VkPipelineVertexInputStateCreateInfo vertex_state {};
        VkPipelineInputAssemblyStateCreateInfo input_assembly_state {};
        VkPipelineMultisampleStateCreateInfo multisample_state {};
        auto& vis = m_vertex_input_states[pp[0]];
        createinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createinfo.stageCount = m_rf_shader_states[pp[1]].first;
        createinfo.pStages = m_rf_shader_states[pp[1]].second;
        createinfo.pVertexInputState = &vertex_state;
        createinfo.pInputAssemblyState = &input_assembly_state;
        createinfo.pTessellationState = &tessellation_state;
        createinfo.pViewportState = &viewport_state;
        createinfo.pRasterizationState = &rasterization_state;
        createinfo.pMultisampleState = &multisample_state;
        createinfo.pDepthStencilState = &depth_stencil_state;
        createinfo.pColorBlendState = &color_blend_state;
        createinfo.pDynamicState = &dynamic_state;
        std::tie(createinfo.layout, createinfo.renderPass, createinfo.subpass) = m_raster_fragment_states[pp[1]];
        vertex_input_state_createinfos(vis.bindings, vis.attributes, vis.topology, vertex_state, input_assembly_state);
        multisample_state_createinfo(m_renderer, multisample_state);

        VkPipeline out;
        VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, nullptr, &out));
        return out;
    }
};

class GPLPipelineFactory final : public twogame::vk::PipelineFactory {
    std::map<uint64_t, VkPipeline> m_vertex_input_states, m_raster_fragment_states;
    std::map<PartialPipeline, VkPipelineLayout> m_raster_fragment_layouts;

public:
    GPLPipelineFactory(const Renderer* r)
        : PipelineFactory(r)
    {
    }

    virtual ~GPLPipelineFactory()
    {
        for (auto it = m_vertex_input_states.begin(); it != m_vertex_input_states.end(); ++it)
            vkDestroyPipeline(m_renderer.device(), it->second, nullptr);
    }

    virtual PartialPipeline vertex_input_state(
        const std::vector<VkVertexInputBindingDescription>& bindings,
        const std::vector<VkVertexInputAttributeDescription>& attributes,
        VkPrimitiveTopology topology)
    {
        uint64_t h = xxh64::hash(reinterpret_cast<const char*>(bindings.data()), sizeof(VkVertexInputBindingDescription) * bindings.size(),
            xxh64::hash(reinterpret_cast<const char*>(attributes.data()), sizeof(VkVertexInputAttributeDescription) * attributes.size(),
                xxh64::hash(reinterpret_cast<const char*>(&topology), sizeof(VkPrimitiveTopology), 0)));

        if (m_vertex_input_states[h] == VK_NULL_HANDLE) {
            VkPipelineVertexInputStateCreateInfo vis_ci {};
            VkPipelineInputAssemblyStateCreateInfo ias_ci {};
            vertex_input_state_createinfos(bindings, attributes, topology, vis_ci, ias_ci);

            VkPipeline out_pipeline;
            VkGraphicsPipelineLibraryCreateInfoEXT libraryinfo {};
            VkGraphicsPipelineCreateInfo createinfo {};
            createinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            createinfo.pNext = &libraryinfo;
            createinfo.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
            createinfo.pVertexInputState = &vis_ci;
            createinfo.pInputAssemblyState = &ias_ci;
            libraryinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
            libraryinfo.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
            VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, nullptr, &out_pipeline));
            m_vertex_input_states[h] = out_pipeline;
        }
        return reinterpret_cast<PartialPipeline>(m_vertex_input_states[h]);
    }

    virtual PartialPipeline raster_and_fragment_state(
        const std::vector<VkPipelineShaderStageCreateInfo>& shaders,
        VkPipelineLayout pipeline_layout,
        VkRenderPass render_pass,
        uint32_t subpass)
    {
        uint64_t h = xxh64::hash((const char*)&pipeline_layout, sizeof(VkPipelineLayout),
            xxh64::hash((const char*)&render_pass, sizeof(VkRenderPass),
                xxh64::hash((const char*)&subpass, sizeof(uint32_t), 0)));

        if (m_raster_fragment_states[h] == VK_NULL_HANDLE) {
            VkPipeline out_pipeline;
            VkGraphicsPipelineLibraryCreateInfoEXT libraryinfo {};
            VkPipelineMultisampleStateCreateInfo multisample_state {};
            VkGraphicsPipelineCreateInfo createinfo {};
            createinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            createinfo.pNext = &libraryinfo;
            createinfo.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
            createinfo.stageCount = shaders.size();
            createinfo.pStages = shaders.data();
            createinfo.pTessellationState = &tessellation_state;
            createinfo.pViewportState = &viewport_state;
            createinfo.pRasterizationState = &rasterization_state;
            createinfo.pMultisampleState = &multisample_state;
            createinfo.pDepthStencilState = &depth_stencil_state;
            createinfo.pColorBlendState = &color_blend_state;
            createinfo.pDynamicState = &dynamic_state;
            createinfo.layout = pipeline_layout;
            createinfo.renderPass = render_pass;
            createinfo.subpass = subpass;
            libraryinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
            libraryinfo.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            multisample_state_createinfo(m_renderer, multisample_state);
            VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, nullptr, &out_pipeline));
            m_raster_fragment_states[h] = out_pipeline;
            m_raster_fragment_layouts[reinterpret_cast<PartialPipeline>(out_pipeline)] = pipeline_layout;
        }
        return reinterpret_cast<PartialPipeline>(m_raster_fragment_states[h]);
    }

    virtual void destroy_raster_and_fragment_state(PartialPipeline fp)
    {
        VkPipeline pipeline = reinterpret_cast<VkPipeline>(fp);
        for (auto it = m_raster_fragment_states.begin(); it != m_raster_fragment_states.end();) {
            if (it->second == pipeline)
                it = m_raster_fragment_states.erase(it);
            else
                ++it;
        }
        m_raster_fragment_layouts.erase(fp);
        vkDestroyPipeline(m_renderer.device(), pipeline, nullptr);
    }

    virtual VkPipeline link_pipeline(const std::array<PartialPipeline, 2>& pp)
    {
        VkPipeline out_pipeline;
        VkGraphicsPipelineCreateInfo createinfo {};
        VkPipelineLibraryCreateInfoKHR libraryinfo {};
        createinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createinfo.pNext = &libraryinfo;
        createinfo.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
        createinfo.layout = m_raster_fragment_layouts[pp[1]];
        libraryinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
        libraryinfo.libraryCount = 2;
        libraryinfo.pLibraries = reinterpret_cast<const VkPipeline*>(pp.data());
        VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, 0, &out_pipeline));
        return out_pipeline;
    }
};

PipelineFactory::~PipelineFactory()
{
    clear_pipelines();
}

PipelineFactory* PipelineFactory::make_pipeline_factory(const Renderer* r, bool graphics_pipeline_library_support)
{
    if (graphics_pipeline_library_support)
        return new GPLPipelineFactory(r);
    else
        return new BasicPipelineFactory(r);
}

VkPipeline PipelineFactory::compute_pipeline(const VkPipelineShaderStageCreateInfo& stage, VkPipelineLayout layout)
{
    VkPipeline out;
    VkComputePipelineCreateInfo cci {};
    cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cci.stage = stage;
    cci.layout = layout;
    VK_CHECK(vkCreateComputePipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &cci, nullptr, &out));
    return out;
}

VkPipeline PipelineFactory::graphics_pipeline(
    PartialPipeline vertex_input,
    PartialPipeline raster_and_fragment)
{
    std::array pk = { vertex_input, raster_and_fragment };
    if (m_pipelines[pk] == VK_NULL_HANDLE)
        m_pipelines[pk] = link_pipeline(pk);
    return m_pipelines[pk];
}

void PipelineFactory::clear_pipelines(const std::set<VkPipeline>& retain)
{
    for (auto it = m_pipelines.begin(); it != m_pipelines.end();) {
        if (retain.count(it->second) > 0)
            ++it;
        else {
            vkDestroyPipeline(m_renderer.device(), it->second, nullptr);
            it = m_pipelines.erase(it);
        }
    }
}

}
