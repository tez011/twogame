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

namespace twogame::asset {

Shader::Shader(const xml::assets::Shader& info, const Renderer* r)
    : m_renderer(*r)
{
    VkResult res;
    std::vector<PHYSFS_File*> inputs;
    PHYSFS_sint64 max_shader_size = 0;
    for (auto it = info.stages().begin(); it != info.stages().end(); ++it) {
        PHYSFS_File* fh = PHYSFS_openRead(it->path().data());
        if (fh) {
            inputs.push_back(fh);
            max_shader_size = std::max(max_shader_size, PHYSFS_fileLength(fh));
        } else {
            throw IOException(it->path(), PHYSFS_getLastErrorCode());
        }
    }

    std::vector<VkDescriptorSetLayoutBinding> material_descriptor_layout;
    std::unique_ptr<uint32_t[]> sbuf = std::make_unique<uint32_t[]>((max_shader_size + 3) >> 2);
    for (size_t i = 0; i < inputs.size(); i++) {
        PHYSFS_sint64 flen = PHYSFS_fileLength(inputs[i]);
        if (PHYSFS_readBytes(inputs[i], sbuf.get(), flen) < flen && !PHYSFS_eof(inputs[i]))
            throw IOException(info.stages().at(i).path(), PHYSFS_getLastErrorCode());

        VkShaderModule smod;
        VkShaderModuleCreateInfo createinfo {};
        createinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createinfo.codeSize = flen;
        createinfo.pCode = sbuf.get();
        if ((res = vkCreateShaderModule(m_renderer.device(), &createinfo, nullptr, &smod)) == VK_ERROR_INVALID_SHADER_NV)
            throw MalformedException(info.stages().at(i).path(), "invalid shader");
        else if (res != VK_SUCCESS)
            std::terminate();
        PHYSFS_close(inputs[i]);

        SpvReflectShaderModule reflect;
        if (spvReflectCreateShaderModule(flen, sbuf.get(), &reflect) != SPV_REFLECT_RESULT_SUCCESS)
            throw MalformedException(info.stages().at(i).path(), "failed to reflect shader");

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
                vk::VertexInput input;
                if (strncmp(iv->name, "in_", 3) == 0) {
                    if (vk::parse(iv->name + 3, input)) {
                        m_inputs[input] = iv->location;
                    } else {
                        std::ostringstream oss;
                        oss << "unrecognizable input: " << iv->name << "@" << iv->location;
                        throw MalformedException(info.name(), "unrecognizable input: {}@{}", iv->name, iv->location);
                    }
                }
            }
        }

        std::vector<SpvReflectDescriptorSet*> sets;
        spvReflectEnumerateDescriptorSets(&reflect, &count, nullptr);
        sets.resize(count);
        spvReflectEnumerateDescriptorSets(&reflect, &count, sets.data());
        for (size_t i = 0; i < sets.size(); i++) {
            if (sets[i]->set >= 4)
                throw MalformedException(info.stages().at(i).path(), "found invalid descriptor set {} >= 4", sets[i]->set);
            if (sets[i]->set != 3)
                continue;

            for (uint32_t j = 0; j < sets[i]->binding_count; j++) {
                VkDescriptorSetLayoutBinding& l_binding = material_descriptor_layout.emplace_back();
                SpvReflectTypeDescription* type_description = sets[i]->bindings[j]->type_description;
                l_binding.binding = sets[i]->bindings[j]->binding;
                l_binding.descriptorType = static_cast<VkDescriptorType>(sets[i]->bindings[j]->descriptor_type);
                l_binding.stageFlags = static_cast<VkShaderStageFlags>(reflect.shader_stage);
                l_binding.descriptorCount = 1;
                for (uint32_t k = 0; k < sets[i]->bindings[j]->array.dims_count; k++)
                    l_binding.descriptorCount *= sets[i]->bindings[j]->array.dims[k];

                if (type_description->type_flags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_IMAGE) {
                    m_material_bindings.emplace(std::piecewise_construct,
                        std::forward_as_tuple(sets[i]->bindings[j]->name),
                        std::forward_as_tuple(l_binding.binding, l_binding.descriptorType, FieldType(), l_binding.descriptorCount, 0));
                } else if (type_description->type_flags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_BLOCK) {
                    if (l_binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                        m_buffer_pools.emplace(std::piecewise_construct, std::forward_as_tuple(l_binding.binding),
                            std::forward_as_tuple(m_renderer, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sets[i]->bindings[j]->block.padded_size, 0x4000));
                    } else if (l_binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        m_buffer_pools.emplace(std::piecewise_construct, std::forward_as_tuple(l_binding.binding),
                            std::forward_as_tuple(m_renderer, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sets[i]->bindings[j]->block.padded_size, 0x1000));
                    }
                    for (uint32_t k = 0; k < sets[i]->bindings[j]->block.member_count; k++) {
                        auto& member = sets[i]->bindings[j]->block.members[k];
                        m_material_bindings.emplace(std::piecewise_construct, std::forward_as_tuple(member.name),
                            std::forward_as_tuple(l_binding.binding, l_binding.descriptorType, FieldType(member.type_description), l_binding.descriptorCount, member.offset));
                    }
                } else {
                    spdlog::critical("unknown material slot encountered");
                    std::terminate();
                }
            }
        }
    }

    VkDescriptorSetLayoutCreateInfo dsl_ci {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = material_descriptor_layout.size();
    dsl_ci.pBindings = material_descriptor_layout.data();
    m_descriptor_pool = new vk::DescriptorPool(m_renderer, dsl_ci, 128);
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
    , m_inputs(std::move(other.m_inputs))
    , m_material_bindings(std::move(other.m_material_bindings))
    , m_descriptor_pool(other.m_descriptor_pool)
    , m_pipeline_layout(other.m_pipeline_layout)
    , m_compute_pipeline(other.m_compute_pipeline)
{
    other.m_descriptor_pool = nullptr;
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
    delete m_descriptor_pool;
    for (auto& stage_info : m_stages)
        vkDestroyShaderModule(m_renderer.device(), stage_info.module, nullptr);
}

VkPipeline Shader::graphics_pipeline(const asset::Mesh* mesh) const
{
    uint64_t mpp = mesh->pipeline_parameter();
    auto pit = m_graphics_pipelines.find(mpp);
    if (pit != m_graphics_pipelines.end()) {
        return pit->second;
    } else {
        std::vector<VkVertexInputAttributeDescription> vertex_attributes(mesh->input_attributes().size());
        for (size_t i = 0; i < vertex_attributes.size(); i++) {
            auto location_it = m_inputs.find(mesh->input_attributes().at(i).field);
            if (location_it == m_inputs.end()) // mesh has an attribute that this shader doesn't process; just ignore it.
                continue;

            vertex_attributes[i].location = location_it->second;
            vertex_attributes[i].binding = mesh->input_attributes().at(i).binding;
            vertex_attributes[i].format = mesh->input_attributes().at(i).format;
            vertex_attributes[i].offset = mesh->input_attributes().at(i).offset;
        }

        VkPipeline pipeline;
        VkGraphicsPipelineCreateInfo createinfo {};
        VkPipelineVertexInputStateCreateInfo vertex_input_state {};
        VkPipelineInputAssemblyStateCreateInfo input_assembly_state {};
        VkPipelineMultisampleStateCreateInfo multisample_state {};
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
        vertex_input_state.vertexBindingDescriptionCount = mesh->input_bindings().size();
        vertex_input_state.pVertexBindingDescriptions = mesh->input_bindings().data();
        vertex_input_state.vertexAttributeDescriptionCount = mesh->input_attributes().size();
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
        VK_CHECK(vkCreateGraphicsPipelines(m_renderer.device(), m_renderer.m_pipeline_cache, 1, &createinfo, nullptr, &pipeline));
        m_graphics_pipelines[mpp] = pipeline;
        return pipeline;
    }
}

enum FieldType_ScalarType {
    FT_VOID = 0,
    FT_BOOL,
    FT_INT32,
    FT_INT64,
    FT_UINT32,
    FT_UINT64,
    FT_FLOAT32,
    FT_FLOAT64,
};
enum FieldType_Dim {
    FTD_SCALAR = 0,
    FTD_VEC2,
    FTD_VEC3,
    FTD_VEC4,
    FTD_MAT2,
    FTD_MAT2x3,
    FTD_MAT2x4,
    FTD_MAT3x2,
    FTD_MAT3,
    FTD_MAT3x4,
    FTD_MAT4x2,
    FTD_MAT4x3,
    FTD_MAT4,
};

Shader::FieldType::FieldType(SpvReflectTypeDescription* td)
    : rep(0)
{
    if (td->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) {
        if (td->traits.numeric.scalar.width == 64)
            f.st = FT_FLOAT64;
        else
            f.st = FT_FLOAT32;
    } else if (td->type_flags & SPV_REFLECT_TYPE_FLAG_INT) {
        if (td->traits.numeric.scalar.signedness) {
            if (td->traits.numeric.scalar.width == 64)
                f.st = FT_INT64;
            else
                f.st = FT_INT32;
        } else {
            if (td->traits.numeric.scalar.width == 64)
                f.st = FT_UINT64;
            else
                f.st = FT_UINT32;
        }
    } else if (td->type_flags & SPV_REFLECT_TYPE_FLAG_BOOL)
        f.st = FT_BOOL;
    else
        f.st = FT_VOID;

    // bounds checking in component_count is already done by the glsl validator
    if (td->type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR)
        f.dim = FTD_VEC2 + (td->traits.numeric.vector.component_count - 2);
    else if (td->type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX)
        f.dim = FTD_MAT2 + ((td->traits.numeric.matrix.column_count - 2) * 3 + (td->traits.numeric.matrix.row_count - 2));
    else
        f.dim = FTD_SCALAR;
}

static size_t expected_tokens(int dim)
{
    switch (dim) {
    case FTD_SCALAR:
        return 1;
    case FTD_VEC2:
    case FTD_VEC3:
    case FTD_VEC4:
        return dim + 1;
    case FTD_MAT2:
    case FTD_MAT2x3:
    case FTD_MAT2x4:
        return (dim - 2) * 2;
    case FTD_MAT3x2:
    case FTD_MAT3:
    case FTD_MAT3x4:
        return (dim - 5) * 3;
    case FTD_MAT4x2:
    case FTD_MAT4x3:
    case FTD_MAT4:
        return (dim - 8) * 4;
    default:
        return std::numeric_limits<size_t>::max();
    }
}

bool Shader::FieldType::parse(const std::string_view& text, void* _out)
{
    if (f.st == FT_VOID)
        return true;

    std::vector<std::string_view> tokens;
    xml::priv::split(tokens, text, " \n\t");
    if (tokens.size() < expected_tokens(f.dim))
        return false;

#define X(F, T, P)                                           \
    if (f.st == (F)) {                                       \
        auto out = reinterpret_cast<T*>(_out);               \
        for (size_t i = 0; i < tokens.size(); i++) {         \
            if (sscanf(tokens[i].data(), "%" P, out++) != 1) \
                return false;                                \
        }                                                    \
    }

    if (f.st == FT_BOOL) {
        auto out = reinterpret_cast<uint32_t*>(_out);
        for (auto it = tokens.begin(); it != tokens.end(); ++it) {
            if ((*it)[0] == 't')
                *out++ = 1;
            else if ((*it)[0] == 'f')
                *out++ = 0;
            else
                return false;
        }
    }
    X(FT_INT32, int32_t, PRIi32);
    X(FT_INT64, int64_t, PRIi64);
    X(FT_UINT32, uint32_t, PRIu32);
    X(FT_UINT64, uint64_t, PRIu64);
    X(FT_FLOAT32, float, "f");
    X(FT_FLOAT64, double, "lf");
#undef X
    return true;
}

}
