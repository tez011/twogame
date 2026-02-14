#include <charconv>
#include <functional>
#include <map>
#include <numeric>
#include "display.h"
#include "spirv_reflect.h"

namespace twogame {

// TODO: this will come from flatbuffers
enum VertexAttributeType {
    Position = 0,
    Normal,
    Tangent,
    UV,
    Color,
    Joints,
    Weights,
};

BufferPool::BufferPool(VkBufferUsageFlags usage, VmaAllocationCreateFlags alloc_flags, size_t unit_size, index_t count)
    : m_unit_size(unit_size)
    , m_count(count)
    , m_usage(usage)
    , m_alloc_flags(alloc_flags)
{
    extend();
}

BufferPool::~BufferPool()
{
    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it)
        vmaDestroyBuffer(DisplayHost::instance().allocator(), std::get<VkBuffer>(*it), std::get<VmaAllocation>(*it));
}

std::vector<bool>::iterator BufferPool::extend()
{
    VkBufferCreateInfo buffer_ci {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = m_unit_size * m_count;
    buffer_ci.usage = m_usage;

    VmaAllocationCreateInfo alloc_ci {};
    alloc_ci.flags = m_alloc_flags | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    auto& out = m_buffers.emplace_back();
    VK_DEMAND(vmaCreateBuffer(DisplayHost::instance().allocator(), &buffer_ci, &alloc_ci, &std::get<VkBuffer>(out), &std::get<VmaAllocation>(out), &std::get<VmaAllocationInfo>(out)));
    m_bits.resize(m_bits.size() + m_count);
    return m_bits.end() - m_count;
}

BufferPool::index_t BufferPool::allocate()
{
    if (m_bits_it == m_bits.end())
        m_bits_it = m_bits.begin();
    if (*m_bits_it) {
        m_bits_it = std::find(m_bits_it, m_bits.end(), false);
        if (m_bits_it == m_bits.end())
            m_bits_it = extend();
    }
    return static_cast<BufferPool::index_t>(std::distance(m_bits.begin(), m_bits_it++));
}

void BufferPool::free(BufferPool::index_t i)
{
    m_bits[i] = false;
    m_bits_it = m_bits.begin() + i;
}

std::tuple<VkBuffer, VkDeviceAddress, VkDeviceSize> BufferPool::buffer_handle(BufferPool::index_t i)
{
    return std::make_tuple(std::get<VkBuffer>(m_buffers[i / m_count]),
        m_unit_size * (i % m_count),
        m_unit_size);
}

std::span<std::byte> BufferPool::buffer_memory(BufferPool::index_t i)
{
    std::byte* address = static_cast<std::byte*>(std::get<VmaAllocationInfo>(m_buffers[i / m_count]).pMappedData);
    return std::span<std::byte>(address + (m_unit_size * (i % m_count)), m_unit_size);
}

DescriptorSet::~DescriptorSet()
{
    if (r_pool)
        r_pool->free(std::move(*this));
}

DescriptorSet::Update::Set::Binding::Binding(DescriptorSet::Update::Set& update, VkWriteDescriptorSet& write, uint32_t binding, uint32_t array_element)
    : r_update(update)
    , m_write(write)
{
    m_write.dstBinding = binding;
    m_write.dstArrayElement = array_element;
    m_write.descriptorType = update.r_set.r_pool->bindings(binding).descriptorType;
}

DescriptorSet::Update& DescriptorSet::Update::Set::Binding::write_image(VkImageView image, VkSampler sampler, VkImageLayout layout)
{
    VkDescriptorImageInfo& image_info = r_update.r_update.m_images.emplace_back();
    if (image == VK_NULL_HANDLE) {
        image_info.sampler = DisplayHost::instance().null_sampler();
        image_info.imageView = DisplayHost::instance().null_image_view();
        image_info.imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    } else {
        image_info.sampler = sampler;
        image_info.imageView = image;
        image_info.imageLayout = layout;
    }
    m_write.pImageInfo = &image_info;
    return r_update.r_update;
}

DescriptorSet::Update& DescriptorSet::Update::Set::Binding::write_buffer(VkBuffer buffer, VkDeviceAddress offset, VkDeviceSize size)
{
    VkDescriptorBufferInfo& buffer_info = r_update.r_update.m_buffers.emplace_back();
    buffer_info.buffer = buffer;
    buffer_info.offset = offset;
    buffer_info.range = size;
    m_write.pBufferInfo = &buffer_info;
    return r_update.r_update;
}

DescriptorSet::Update& DescriptorSet::Update::Set::Binding::write_buffer(std::tuple<VkBuffer, VkDeviceAddress, VkDeviceSize> args)
{
    using write_buffer_t = twogame::DescriptorSet::Update& (twogame::DescriptorSet::Update::Set::Binding::*)(VkBuffer buffer, VkDeviceAddress offset, VkDeviceSize size);
    return std::apply(std::bind_front(static_cast<write_buffer_t>(&DescriptorSet::Update::Set::Binding::write_buffer), this), args);
}

DescriptorSet::Update::Set::Set(DescriptorSet::Update& update, VkWriteDescriptorSet& write, DescriptorSet& set)
    : r_update(update)
    , r_set(set)
    , m_write(write)
{
    write.dstSet = set;
}

DescriptorSet::Update::Set::Binding DescriptorSet::Update::Set::binding(uint32_t binding, uint32_t array_element)
{
    return DescriptorSet::Update::Set::Binding(*this, m_write, binding, array_element);
}

DescriptorSet::Update::Set DescriptorSet::Update::set(DescriptorSet& set)
{
    VkWriteDescriptorSet& write = m_writes.emplace_back();
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.descriptorCount = 1;

    return Set(*this, write, set);
}

void DescriptorSet::Update::finish()
{
    vkUpdateDescriptorSets(DisplayHost::instance().device(), m_writes.size(), m_writes.data(), 0, nullptr);
}

DescriptorPool::DescriptorPool(VkDescriptorSetLayout layout, const std::vector<DescriptorBindingInfo>& bindings)
    : r_layout(layout)
    , m_bindings(bindings)
    , m_alloc_count(0)
{
    std::vector<VkDescriptorPoolSize> descriptor_pool_sizes;
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        VkDescriptorPoolSize& ps = descriptor_pool_sizes.emplace_back();
        ps.type = it->descriptorType;
        ps.descriptorCount = it->descriptorCount;
        if (ps.type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
            ps.descriptorCount *= it->uniform_buffer_size;
    }

    VkDescriptorPoolCreateInfo pool_ci {};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = DESCRIPTOR_SETS_PER_POOL;
    pool_ci.poolSizeCount = descriptor_pool_sizes.size();
    pool_ci.pPoolSizes = descriptor_pool_sizes.data();
    VK_DEMAND(vkCreateDescriptorPool(DisplayHost::instance().device(), &pool_ci, nullptr, &m_pool));
}

DescriptorPool::~DescriptorPool()
{
    vkDestroyDescriptorPool(DisplayHost::instance().device(), m_pool, nullptr);
}

DescriptorSet DescriptorPool::allocate()
{
    if (m_free_sets.empty() == false) {
        DescriptorSet set = std::move(m_free_sets.back());
        m_free_sets.pop_back();
        return set;
    } else if (m_alloc_count < DESCRIPTOR_SETS_PER_POOL) {
        VkDescriptorSetAllocateInfo alloc_info {};
        VkDescriptorSet descriptor_set;
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = m_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &r_layout;
        VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::instance().device(), &alloc_info, &descriptor_set));

        return DescriptorSet(*this, descriptor_set);
    } else {
        std::abort();
    }
}

void DescriptorPool::free(DescriptorSet&& set)
{
    m_free_sets.push_back(set);
}

Pipeline::Pipeline(Pipeline&& other)
    : m_descriptor_set_layouts(other.m_descriptor_set_layouts)
    , m_descriptor_pools(std::move(other.m_descriptor_pools))
    , m_layout(other.m_layout)
    , m_pipeline(other.m_pipeline)
{
    for (size_t i = 0; i < m_descriptor_bindings.size(); i++)
        m_descriptor_bindings[i] = std::move(other.m_descriptor_bindings[i]);

    other.m_pipeline = VK_NULL_HANDLE;
    other.m_layout = VK_NULL_HANDLE;
    other.m_descriptor_set_layouts.fill(VK_NULL_HANDLE);
}

Pipeline::~Pipeline()
{
    for (auto it = m_descriptor_pools.begin(); it != m_descriptor_pools.end(); ++it)
        it->clear();
    vkDestroyPipeline(DisplayHost::instance().device(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(DisplayHost::instance().device(), m_layout, nullptr);
    for (auto it = m_descriptor_set_layouts.begin(); it != m_descriptor_set_layouts.end(); ++it) {
        if (*it != DisplayHost::instance().empty_descriptor_set_layout())
            vkDestroyDescriptorSetLayout(DisplayHost::instance().device(), *it, nullptr);
    }
}

void Pipeline::create_layout(const std::array<std::vector<DescriptorBindingInfo>, 4>& descriptor_bindings, const std::vector<VkPushConstantRange>& push_constant_ranges)
{
    m_descriptor_bindings = descriptor_bindings;
    for (size_t set = 0; set < descriptor_bindings.size(); set++) {
        if (descriptor_bindings[set].empty()) {
            m_descriptor_set_layouts[set] = DisplayHost::instance().empty_descriptor_set_layout();
        } else {
            VkDescriptorSetLayoutCreateInfo layout_ci {};
            layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_ci.bindingCount = descriptor_bindings[set].size();
            layout_ci.pBindings = descriptor_bindings[set].data();
            VK_DEMAND(vkCreateDescriptorSetLayout(DisplayHost::instance().device(), &layout_ci, nullptr, &m_descriptor_set_layouts[set]));
            m_descriptor_pools[set].emplace_back(m_descriptor_set_layouts[set], descriptor_bindings[set]);
        }
    }

    VkPipelineLayoutCreateInfo pipeline_layout_ci {};
    pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_ci.setLayoutCount = m_descriptor_set_layouts.size();
    pipeline_layout_ci.pSetLayouts = m_descriptor_set_layouts.data();
    pipeline_layout_ci.pushConstantRangeCount = push_constant_ranges.size();
    pipeline_layout_ci.pPushConstantRanges = push_constant_ranges.data();
    VK_DEMAND(vkCreatePipelineLayout(DisplayHost::instance().device(), &pipeline_layout_ci, nullptr, &m_layout));
}

void Pipeline::create_pipeline(VkGraphicsPipelineCreateInfo& createinfo, VkPipelineCache cache)
{
    createinfo.layout = m_layout;
    VK_DEMAND(vkCreateGraphicsPipelines(DisplayHost::instance().device(), cache, 1, &createinfo, nullptr, &m_pipeline));
}

void Pipeline::create_pipeline(VkComputePipelineCreateInfo& createinfo, VkPipelineCache cache)
{
    createinfo.layout = m_layout;
    VK_DEMAND(vkCreateComputePipelines(DisplayHost::instance().device(), cache, 1, &createinfo, nullptr, &m_pipeline));
}

DescriptorSet Pipeline::allocate_descriptor_set(int set)
{
    if (m_descriptor_pools[set].back().full())
        m_descriptor_pools[set].emplace_back(m_descriptor_set_layouts[set], m_descriptor_bindings[set]);

    return m_descriptor_pools[set].back().allocate();
}

void PipelineBuilder::reset(bool is_graphics)
{
    m_is_graphics = is_graphics;
    m_shader_modules_ci.clear();
    m_pipeline_shaders.clear();
    m_shader_specializations.clear();
}

PipelineBuilder& PipelineBuilder::new_graphics(VkRenderPass render_pass, uint32_t subpass)
{
    reset(true);
    m_primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    m_depth_clamp = VK_FALSE;
    m_render_pass = { render_pass, subpass };
    return *this;
}

PipelineBuilder& PipelineBuilder::new_compute()
{
    reset(false);
    return *this;
}

PipelineBuilder& PipelineBuilder::with_shader(const uint32_t* text, size_t size, VkShaderStageFlagBits stage, const void* specialization, size_t specialization_size, std::span<const VkSpecializationMapEntry> specialization_map)
{
    VkShaderModuleCreateInfo& sm_ci = m_shader_modules_ci.emplace_back();
    sm_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = size;
    sm_ci.pCode = text;

    VkPipelineShaderStageCreateInfo& shader_ci = m_pipeline_shaders.emplace_back();
    shader_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_ci.stage = stage;
    shader_ci.module = reinterpret_cast<VkShaderModule>(m_shader_modules_ci.size());
    shader_ci.pName = "main";
    if (specialization) {
        VkSpecializationInfo& specialization_info = m_shader_specializations.emplace_back();
        specialization_info.mapEntryCount = specialization_map.size();
        specialization_info.pMapEntries = specialization_map.data();
        specialization_info.dataSize = specialization_size;
        specialization_info.pData = specialization;
        shader_ci.pSpecializationInfo = reinterpret_cast<VkSpecializationInfo*>(m_shader_specializations.size());
    } else {
        shader_ci.pSpecializationInfo = nullptr;
    }
    return *this;
}

PipelineBuilder& PipelineBuilder::with_depth_clamp(bool depth_clamp)
{
    m_depth_clamp = depth_clamp ? VK_TRUE : VK_FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::with_primitive_topology(VkPrimitiveTopology topology)
{
    m_primitive_topology = topology;
    return *this;
}

static std::pair<uint32_t, uint32_t> parse_reflect_name(std::string_view name)
{
    uint32_t category, index;
    std::string_view::reverse_iterator suffix_rev = name.rbegin();
    while (isdigit(*suffix_rev))
        ++suffix_rev;
    if (std::from_chars(suffix_rev.base(), name.end(), index).ec != std::errc {})
        index = 0;
    if (name.starts_with("in_"))
        name = name.substr(3);

    std::array<char, 16> prefix;
    for (size_t i = 0; i < std::min(name.size(), prefix.size()); i++)
        prefix[i] = tolower(name[i]);
    if (strncmp(prefix.data(), "position", 8) == 0)
        category = VertexAttributeType::Position;
    else if (strncmp(prefix.data(), "normal", 6) == 0)
        category = VertexAttributeType::Normal;
    else if (strncmp(prefix.data(), "tangent", 7) == 0)
        category = VertexAttributeType::Tangent;
    else if (strncmp(prefix.data(), "uv", 2) == 0)
        category = VertexAttributeType::UV;
    else if (strncmp(prefix.data(), "joints", 6) == 0)
        category = VertexAttributeType::Joints;
    else if (strncmp(prefix.data(), "weights", 7) == 0)
        category = VertexAttributeType::Weights;
    else
        category = UINT32_MAX;
    return std::make_pair(category, index);
}

Pipeline PipelineBuilder::build()
{
    std::vector<VkShaderModule> shader_modules(m_shader_modules_ci.size());
    std::array<std::vector<DescriptorBindingInfo>, 4> descriptor_bindings;
    std::vector<VkPushConstantRange> push_constant_ranges;
    std::vector<VkVertexInputAttributeDescription> vertex_input_atts;
    std::vector<VkVertexInputBindingDescription> vertex_input_bindings;
    for (size_t i = 0; i < m_shader_modules_ci.size(); i++) {
        uint32_t count = 0;
        SpvReflectShaderModule mod;
        SpvReflectResult res = spvReflectCreateShaderModule(m_shader_modules_ci[i].codeSize, m_shader_modules_ci[i].pCode, &mod);
        SDL_assert_release(res == SPV_REFLECT_RESULT_SUCCESS);

        std::vector<SpvReflectDescriptorBinding*> reflect_descriptor_bindings;
        spvReflectEnumerateDescriptorBindings(&mod, &count, nullptr);
        reflect_descriptor_bindings.resize(count);
        res = spvReflectEnumerateDescriptorBindings(&mod, &count, reflect_descriptor_bindings.data());
        SDL_assert_release(res == SPV_REFLECT_RESULT_SUCCESS);
        for (auto it = reflect_descriptor_bindings.begin(); it != reflect_descriptor_bindings.end(); ++it) {
            SDL_assert((*it)->set < 4);

            DescriptorBindingInfo& b = descriptor_bindings[(*it)->set].emplace_back();
            b.binding = (*it)->binding;
            b.descriptorType = static_cast<VkDescriptorType>((*it)->descriptor_type);
            b.descriptorCount = (*it)->count;
            b.stageFlags = static_cast<VkShaderStageFlags>(mod.shader_stage);
            b.pImmutableSamplers = nullptr;

            if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                b.uniform_buffer_size = (*it)->block.size; // could be zero, if runtime-sized storage buffer
                if (std::string_view((*it)->type_description->type_name).starts_with("inline_"))
                    b.descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
            }
        }

        std::vector<SpvReflectBlockVariable*> reflect_push_constants;
        spvReflectEnumeratePushConstantBlocks(&mod, &count, nullptr);
        reflect_push_constants.resize(count);
        res = spvReflectEnumeratePushConstantBlocks(&mod, &count, reflect_push_constants.data());
        SDL_assert_release(res == SPV_REFLECT_RESULT_SUCCESS);
        for (auto it = reflect_push_constants.begin(); it != reflect_push_constants.end(); ++it) {
            VkPushConstantRange& range = push_constant_ranges.emplace_back();
            range.stageFlags = static_cast<VkShaderStageFlags>(mod.shader_stage);
            range.offset = (*it)->absolute_offset;
            range.size = (*it)->size;
        }

        if (mod.shader_stage & SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
            std::vector<SpvReflectInterfaceVariable*> reflect_input_vars;
            spvReflectEnumerateInputVariables(&mod, &count, nullptr);
            reflect_input_vars.resize(count);
            res = spvReflectEnumerateInputVariables(&mod, &count, reflect_input_vars.data());
            SDL_assert_release(res == SPV_REFLECT_RESULT_SUCCESS);

            std::map<uint32_t, std::vector<VkVertexInputAttributeDescription>> input_bindings;
            for (auto it = reflect_input_vars.begin(); it != reflect_input_vars.end(); ++it) {
                if ((*it)->location == UINT32_MAX)
                    continue;
                std::pair<uint32_t, uint32_t> name = parse_reflect_name((*it)->name);
                if (name.first == UINT32_MAX) {
                    SDL_LogError(SDL_LOG_CATEGORY_GPU, "unrecognized vertex input %s in shader; skipping", (*it)->name);
                    continue;
                }

                VkVertexInputAttributeDescription& att = input_bindings[name.first].emplace_back();
                att.location = (*it)->location;
                std::tie(att.binding, att.offset) = name; // for now, att.offset contains the index, NOT the offset
                att.format = static_cast<VkFormat>((*it)->format);
            }
            // Calculate real offsets
            for (auto it = input_bindings.begin(); it != input_bindings.end(); ++it) {
                VkDeviceAddress c_offset = 0;
                std::sort(it->second.begin(), it->second.end(), [](const VkVertexInputAttributeDescription& lhs, const VkVertexInputAttributeDescription& rhs) {
                    return lhs.offset < rhs.offset;
                });
                for (auto bt = it->second.begin(); bt != it->second.end(); ++bt) {
                    bt->offset = c_offset;
                    c_offset += DisplayHost::format_width(bt->format);
                }
                VkVertexInputBindingDescription& vertex_binding = vertex_input_bindings.emplace_back();
                vertex_binding.binding = it->first;
                vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                vertex_binding.stride = c_offset;
                vertex_input_atts.insert(vertex_input_atts.end(), it->second.begin(), it->second.end());
            }
        }

        VK_DEMAND(vkCreateShaderModule(DisplayHost::instance().device(), &m_shader_modules_ci[i], nullptr, &shader_modules[i]));
    }

    // All structures containing pointers that are actually indexes need to be updated
    for (auto it = m_pipeline_shaders.begin(); it != m_pipeline_shaders.end(); ++it) {
        if (it->pSpecializationInfo != nullptr)
            it->pSpecializationInfo = &m_shader_specializations[reinterpret_cast<size_t>(it->pSpecializationInfo)];
        it->module = shader_modules[reinterpret_cast<size_t>(it->module) - 1];
    }

    Pipeline out_pipeline;
    out_pipeline.create_layout(descriptor_bindings, push_constant_ranges);
    if (m_is_graphics) {
        VkPipelineVertexInputStateCreateInfo vertex_input_info {};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = vertex_input_bindings.size();
        vertex_input_info.pVertexBindingDescriptions = vertex_input_bindings.data();
        vertex_input_info.vertexAttributeDescriptionCount = vertex_input_atts.size();
        vertex_input_info.pVertexAttributeDescriptions = vertex_input_atts.data();

        VkPipelineInputAssemblyStateCreateInfo input_assy_info {};
        input_assy_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assy_info.topology = m_primitive_topology;

        VkPipelineViewportStateCreateInfo viewport_info {};
        viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_info.viewportCount = 1;
        viewport_info.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer_info {};
        rasterizer_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer_info.depthClampEnable = m_depth_clamp;
        rasterizer_info.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer_info.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer_info.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample_info {};
        multisample_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_info.sampleShadingEnable = VK_FALSE;
        multisample_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
        depth_stencil_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_info.depthTestEnable = VK_TRUE;
        depth_stencil_info.depthWriteEnable = VK_TRUE;
        depth_stencil_info.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        depth_stencil_info.depthBoundsTestEnable = VK_FALSE;
        depth_stencil_info.stencilTestEnable = VK_FALSE;

        std::array<VkPipelineColorBlendAttachmentState, 1> color_blend_atts {};
        VkPipelineColorBlendStateCreateInfo color_blend_info {};
        color_blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_info.attachmentCount = color_blend_atts.size();
        color_blend_info.pAttachments = color_blend_atts.data();
        color_blend_atts[0].blendEnable = VK_FALSE;
        color_blend_atts[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineDynamicStateCreateInfo dynamic_state_info {};
        auto dynamic_state_set = std::to_array<VkDynamicState>({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR });
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = dynamic_state_set.size();
        dynamic_state_info.pDynamicStates = dynamic_state_set.data();

        VkGraphicsPipelineCreateInfo pipeline_ci {};
        pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_ci.stageCount = m_pipeline_shaders.size();
        pipeline_ci.pStages = m_pipeline_shaders.data();
        pipeline_ci.pVertexInputState = &vertex_input_info;
        pipeline_ci.pInputAssemblyState = &input_assy_info;
        pipeline_ci.pViewportState = &viewport_info;
        pipeline_ci.pRasterizationState = &rasterizer_info;
        pipeline_ci.pMultisampleState = &multisample_info;
        pipeline_ci.pDepthStencilState = &depth_stencil_info;
        pipeline_ci.pColorBlendState = &color_blend_info;
        pipeline_ci.pDynamicState = &dynamic_state_info;
        pipeline_ci.renderPass = m_render_pass.first;
        pipeline_ci.subpass = m_render_pass.second;
        out_pipeline.create_pipeline(pipeline_ci, DisplayHost::instance().pipeline_cache());
    } else {
        VkComputePipelineCreateInfo pipeline_ci {};
        pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_ci.stage = m_pipeline_shaders.front();
        out_pipeline.create_pipeline(pipeline_ci, DisplayHost::instance().pipeline_cache());
    }

    for (auto it = shader_modules.begin(); it != shader_modules.end(); ++it)
        vkDestroyShaderModule(DisplayHost::instance().device(), *it, nullptr);
    reset(m_is_graphics);
    return out_pipeline;
}

}
