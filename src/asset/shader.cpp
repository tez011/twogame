#include <memory>
#include <sstream>
#include <physfs.h>
#include <spirv_reflect.h>
#include "asset.h"
#include "render.h"
#include "xml.h"

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
    VK_COMPARE_OP_LESS_OR_EQUAL, // depthCompareOp
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

namespace twogame::asset {

Shader::Shader(const xml::assets::Shader& info, const AssetManager& manager)
    : m_renderer(manager.renderer())
{
    VkResult res;
    std::vector<PHYSFS_File*> inputs;
    PHYSFS_sint64 max_shader_size = 0;
    for (auto it = info.stages().begin(); it != info.stages().end(); ++it) {
        PHYSFS_File* fh = PHYSFS_openRead(it->source().data());
        if (fh) {
            inputs.push_back(fh);
            max_shader_size = std::max(max_shader_size, PHYSFS_fileLength(fh));
        } else {
            throw IOException(it->source(), PHYSFS_getLastErrorCode());
        }
    }

    uint32_t max_descriptor_binding = 0;
    std::map<uint32_t, DescriptorBinding> descriptor_bindings_init;
    std::vector<VkDescriptorSetLayoutBinding> material_descriptor_layout;
    std::unique_ptr<uint32_t[]> sbuf = std::make_unique<uint32_t[]>((max_shader_size + 3) >> 2);
    for (size_t i = 0; i < inputs.size(); i++) {
        PHYSFS_sint64 flen = PHYSFS_fileLength(inputs[i]);
        if (PHYSFS_readBytes(inputs[i], sbuf.get(), flen) < flen && !PHYSFS_eof(inputs[i]))
            throw IOException(info.stages().at(i).source(), PHYSFS_getLastErrorCode());

        VkShaderModule smod;
        VkShaderModuleCreateInfo createinfo {};
        createinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createinfo.codeSize = flen;
        createinfo.pCode = sbuf.get();
        if ((res = vkCreateShaderModule(m_renderer.device(), &createinfo, nullptr, &smod)) == VK_ERROR_INVALID_SHADER_NV)
            throw MalformedException(info.stages().at(i).source(), "invalid shader");
        else if (res != VK_SUCCESS)
            std::terminate();
        PHYSFS_close(inputs[i]);

        SpvReflectShaderModule reflect;
        if (spvReflectCreateShaderModule(flen, sbuf.get(), &reflect) != SPV_REFLECT_RESULT_SUCCESS)
            throw MalformedException(info.stages().at(i).source(), "failed to reflect shader");

        VkPipelineShaderStageCreateInfo& stage_info = m_stages.emplace_back();
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = static_cast<VkShaderStageFlagBits>(reflect.shader_stage);
        stage_info.module = smod;
        stage_info.pName = "main";
        stage_info.pSpecializationInfo = nullptr;

        uint32_t count;
        if (stage_info.stage == VK_SHADER_STAGE_VERTEX_BIT) {
            std::vector<SpvReflectInterfaceVariable*> inputs;
            spvReflectEnumerateInputVariables(&reflect, &count, nullptr);
            inputs.resize(count);
            spvReflectEnumerateInputVariables(&reflect, &count, inputs.data());

            for (auto* iv : inputs) {
                if (strncmp(iv->name, "in_", 3) == 0) {
                    m_input_attributes[iv->name + 3] = iv->location;
                }
            }
        }

        std::vector<SpvReflectDescriptorSet*> sets;
        spvReflectEnumerateDescriptorSets(&reflect, &count, nullptr);
        sets.resize(count);
        spvReflectEnumerateDescriptorSets(&reflect, &count, sets.data());
        for (size_t i = 0; i < sets.size(); i++) {
            if (sets[i]->set != 3)
                continue;

            for (uint32_t j = 0; j < sets[i]->binding_count; j++) {
                VkDescriptorSetLayoutBinding& l_binding = material_descriptor_layout.emplace_back();
                l_binding.binding = sets[i]->bindings[j]->binding;
                l_binding.descriptorType = static_cast<VkDescriptorType>(sets[i]->bindings[j]->descriptor_type);
                l_binding.stageFlags = static_cast<VkShaderStageFlags>(reflect.shader_stage);
                l_binding.descriptorCount = 1;

                max_descriptor_binding = std::max(max_descriptor_binding, l_binding.binding);
                descriptor_bindings_init[l_binding.binding].type = l_binding.descriptorType;
                descriptor_bindings_init[l_binding.binding].offset = 0;
                descriptor_bindings_init[l_binding.binding].range = 0;

                SpvReflectTypeFlags tflags = sets[i]->bindings[j]->type_description->type_flags;
                if (tflags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_IMAGE) {
                    m_material_bindings[sets[i]->bindings[j]->name].binding = l_binding.binding;
                    m_material_bindings[sets[i]->bindings[j]->name].offset = 0;
                    m_material_bindings[sets[i]->bindings[j]->name].range = 0;
                } else if (tflags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_BLOCK) {
                    descriptor_bindings_init[l_binding.binding].range = sets[i]->bindings[j]->block.padded_size;
                    for (uint32_t k = 0; k < sets[i]->bindings[j]->block.member_count; k++) {
                        const auto& member = sets[i]->bindings[j]->block.members[k];
                        m_material_bindings[member.name].binding = l_binding.binding;
                        m_material_bindings[member.name].offset = member.offset;
                        m_material_bindings[member.name].range = member.size;
                    }
                } else {
                    throw MalformedException(info.name(), "unknown SPIRV type flags {}", tflags);
                }
            }
        }
    }

    m_descriptor_bindings.resize(max_descriptor_binding + 1);
    for (size_t i = 0; i < m_descriptor_bindings.size(); i++) {
        m_descriptor_bindings[i].offset = 0;
        if (descriptor_bindings_init.contains(i)) {
            m_descriptor_bindings[i].type = descriptor_bindings_init[i].type;
            m_descriptor_bindings[i].range = (descriptor_bindings_init[i].range + 15) & ~15;
        } else {
            m_descriptor_bindings[i].type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            m_descriptor_bindings[i].range = 0;
        }
    }

    m_material_buffer_size = 0;
    for (size_t i = 0; i < m_descriptor_bindings.size(); i++) {
        if (i > 0)
            m_descriptor_bindings[i].offset = m_material_buffer_size + m_descriptor_bindings[i - 1].offset;
        m_material_buffer_size += m_descriptor_bindings[i].range;
    }

    VkDescriptorSetLayoutCreateInfo dsl_ci {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = material_descriptor_layout.size();
    dsl_ci.pBindings = material_descriptor_layout.data();
    m_descriptor_pool = std::make_unique<vk::DescriptorPool>(m_renderer, dsl_ci, 128);
    m_pipeline_layout = m_renderer.create_pipeline_layout(m_descriptor_pool->layout());

    if (m_stages.size() == 1 && m_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        VkComputePipelineCreateInfo cci {};
        cci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cci.stage = m_stages[0];
        cci.layout = m_pipeline_layout;
        VK_CHECK(vkCreateComputePipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &cci, nullptr, &m_compute_pipeline));
    } else {
        m_compute_pipeline = VK_NULL_HANDLE;
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_renderer(other.m_renderer)
    , m_stages(std::move(other.m_stages))
    , m_input_attributes(std::move(other.m_input_attributes))
    , m_material_bindings(std::move(other.m_material_bindings))
    , m_pipeline_layout(other.m_pipeline_layout)
    , m_compute_pipeline(other.m_compute_pipeline)
    , m_descriptor_pool(std::move(other.m_descriptor_pool))
{
    other.m_pipeline_layout = VK_NULL_HANDLE;
    other.m_compute_pipeline = VK_NULL_HANDLE;
}

Shader::~Shader()
{
    if (m_compute_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_renderer.device(), m_compute_pipeline, nullptr);
    for (auto it = m_graphics_pipelines.begin(); it != m_graphics_pipelines.end(); ++it)
        vkDestroyPipeline(m_renderer.device(), it->second, nullptr);

    vkDestroyPipelineLayout(m_renderer.device(), m_pipeline_layout, nullptr);
    m_descriptor_pool.reset();
    for (auto& stage_info : m_stages)
        vkDestroyShaderModule(m_renderer.device(), stage_info.module, nullptr);
}

VkPipeline Shader::graphics_pipeline(const Mesh* mesh, const Material*, size_t primitive_group_index) const
{
    const auto& pg = mesh->primitive_group(primitive_group_index);
    auto pit = m_graphics_pipelines.find(pg.pipeline_parameter);
    if (pit != m_graphics_pipelines.end())
        return pit->second;

    std::vector<VkVertexInputAttributeDescription> vertex_attributes(pg.attributes);
    for (size_t i = 0; i < vertex_attributes.size(); i++) {
        auto location_it = m_input_attributes.find(pg.attribute_names[vertex_attributes[i].location]);
        if (location_it != m_input_attributes.end())
            vertex_attributes[i].location = location_it->second;
    }

    VkPipeline pipeline;
    VkGraphicsPipelineCreateInfo createinfo {};
    VkPipelineVertexInputStateCreateInfo vertex_input_state {};
    VkPipelineInputAssemblyStateCreateInfo input_assembly_state {};
    VkPipelineMultisampleStateCreateInfo multisample_state {};
    VkPipelineRasterizationStateCreateInfo rasterization_state {};
    createinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    createinfo.stageCount = m_stages.size();
    createinfo.pStages = m_stages.data();
    createinfo.pVertexInputState = &vertex_input_state;
    createinfo.pInputAssemblyState = &input_assembly_state;
    createinfo.pTessellationState = &tessellation_state;
    createinfo.pViewportState = &viewport_state;
    createinfo.pRasterizationState = &rasterization_state;
    createinfo.pMultisampleState = &multisample_state;
    createinfo.pDepthStencilState = &depth_stencil_state;
    createinfo.pColorBlendState = &color_blend_state;
    createinfo.pDynamicState = &dynamic_state;
    createinfo.layout = m_pipeline_layout;
    createinfo.renderPass = m_renderer.m_render_pass[0];
    createinfo.subpass = 0;
    vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_state.vertexBindingDescriptionCount = pg.bindings.size();
    vertex_input_state.pVertexBindingDescriptions = pg.bindings.data();
    vertex_input_state.vertexAttributeDescriptionCount = vertex_attributes.size();
    vertex_input_state.pVertexAttributeDescriptions = vertex_attributes.data();
    input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_state.topology = mesh->primitive_topology();
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = static_cast<VkSampleCountFlagBits>(m_renderer.m_multisample_count),
    multisample_state.sampleShadingEnable = m_renderer.m_sample_shading > 0 ? VK_TRUE : VK_FALSE;
    multisample_state.minSampleShading = m_renderer.m_sample_shading;
    multisample_state.pSampleMask = nullptr;
    multisample_state.alphaToCoverageEnable = VK_FALSE;
    multisample_state.alphaToOneEnable = VK_FALSE;
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization_state.lineWidth = 1.f;
    VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, nullptr, &pipeline));
    m_graphics_pipelines[pg.pipeline_parameter] = pipeline;
    return pipeline;
}

}
