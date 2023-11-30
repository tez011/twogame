#include <memory>
#include <sstream>
#include <physfs.h>
#include <spirv_reflect.h>
#include "asset.h"
#include "render.h"
#include "xml.h"

using namespace std::literals;

namespace twogame::asset {

#define INPUT_LOCATIONS   \
    X(Position, position) \
    X(Normal, normal)     \
    X(UV0, texcoord0)
static bool check_input_location(SpvReflectInterfaceVariable* iv)
{
#define X(LN, NAME)                                                                                         \
    if (strcmp(iv->name, "in_" #NAME) == 0 && iv->location == static_cast<size_t>(Shader::VertexInput::LN)) \
        return true;
    INPUT_LOCATIONS
#undef X

    return false;
}

Shader::VertexInput Shader::input_location(const std::string_view& name)
{
#define X(LN, NAME)    \
    if (name == #NAME) \
        return VertexInput::LN;
    INPUT_LOCATIONS
#undef X

    return VertexInput::MAX_VALUE;
}

Shader::Shader(const xml::assets::Shader& info, const Renderer* r)
    : AbstractAsset(r)
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
                if (check_input_location(iv))
                    m_inputs[iv->location] = static_cast<VkFormat>(iv->format);
                else {
                    std::ostringstream oss;
                    oss << "bad input location: " << iv->name << "@" << iv->location;
                    throw MalformedException(info.name(), oss.str());
                }
            }
        }

        std::vector<SpvReflectDescriptorSet*> sets;
        spvReflectEnumerateDescriptorSets(&reflect, &count, nullptr);
        sets.resize(count);
        spvReflectEnumerateDescriptorSets(&reflect, &count, sets.data());
        for (size_t i = 0; i < sets.size(); i++) {
            if (sets[i]->set >= 4)
                throw MalformedException(info.stages().at(i).path(), "found invalid descriptor set >= 4");
            if (sets[i]->set != 3)
                continue;

            for (uint32_t j = 0; j < sets[i]->binding_count; j++) {
                VkDescriptorSetLayoutBinding& l_binding = material_descriptor_layout.emplace_back();
                l_binding.binding = sets[i]->bindings[j]->binding;
                l_binding.descriptorType = static_cast<VkDescriptorType>(sets[i]->bindings[j]->descriptor_type);
                l_binding.stageFlags = static_cast<VkShaderStageFlags>(reflect.shader_stage);
                l_binding.descriptorCount = 1;
                for (uint32_t k = 0; k < sets[i]->bindings[j]->array.dims_count; k++)
                    l_binding.descriptorCount *= sets[i]->bindings[j]->array.dims[k];

                SpvReflectTypeDescription* type_description = sets[i]->bindings[j]->type_description;
                if (type_description->type_flags & SPV_REFLECT_TYPE_FLAG_EXTERNAL_IMAGE)
                    m_material_bindings.emplace(std::piecewise_construct,
                        std::forward_as_tuple(sets[i]->bindings[j]->name),
                        std::forward_as_tuple(l_binding.binding, l_binding.descriptorType, l_binding.descriptorCount, 0, 0));
                else {
                    spdlog::critical("unknown material slot encountered. Engine developer, that's on you!");
                    std::terminate();
                }
            }
        }
    }

    VkDescriptorSetLayoutCreateInfo dsl_ci {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = material_descriptor_layout.size();
    dsl_ci.pBindings = material_descriptor_layout.data();
    m_descriptor_pool = new vk::DescriptorPool(m_renderer, dsl_ci);
    m_pipeline_layout = m_renderer.create_pipeline_layout(m_descriptor_pool->layout());

    if (m_stages.size() == 1 && m_stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        m_compute_pipeline = m_renderer.pipeline_factory().compute_pipeline(m_stages[0], m_pipeline_layout);
    } else {
        m_compute_pipeline = VK_NULL_HANDLE;

        for (auto& rpi : info.render_passes()) {
            VkRenderPass render_pass = m_renderer.render_pass(rpi.render_pass_index());
            m_graphics_pipelines[std::make_pair(render_pass, rpi.subpass_index())] = m_renderer.pipeline_factory().raster_and_fragment_state(m_stages, m_pipeline_layout, render_pass, rpi.subpass_index());
        }
    }
}

Shader::Shader(Shader&& other) noexcept
    : AbstractAsset(&other.m_renderer)
    , m_stages(std::move(other.m_stages))
    , m_inputs(std::move(other.m_inputs))
    , m_material_bindings(std::move(other.m_material_bindings))
    , m_graphics_pipelines(std::move(other.m_graphics_pipelines))
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
        m_renderer.pipeline_factory().destroy_raster_and_fragment_state(it->second);

    vkDestroyPipelineLayout(m_renderer.device(), m_pipeline_layout, nullptr);
    delete m_descriptor_pool;
    for (auto& stage_info : m_stages)
        vkDestroyShaderModule(m_renderer.device(), stage_info.module, nullptr);
}

}
