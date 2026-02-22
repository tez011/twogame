#include <set>
#include "display.h"
#include "embedded_shaders.h"
#include "scene.h"

namespace twogame {

IRenderer::IRenderer()
    : m_perspective_projection(GLMS_MAT4_ZERO_INIT)
    , m_ortho_projection(GLMS_MAT4_ZERO_INIT)
    , m_descriptor_layouts(3)
    , m_render_pass(VK_NULL_HANDLE)
{
    VkPhysicalDeviceProperties hwd_props;
    vkGetPhysicalDeviceProperties(DisplayHost::hardware_device(), &hwd_props);

    VkBufferCreateInfo buffer_ci {};
    VmaAllocationCreateInfo alloc_ci {};
    VmaAllocationInfo alloc_info;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = 4 * sizeof(mat4);
    buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_uniform_buffer, &m_uniform_buffer_mem, &alloc_info));
    m_uniform_buffer_ptr = static_cast<std::byte*>(alloc_info.pMappedData);

    VkSamplerCreateInfo sampler_info {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = std::min(8.f, hwd_props.limits.maxSamplerAnisotropy);
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_DEMAND(vkCreateSampler(DisplayHost::device(), &sampler_info, nullptr, &m_sampler));

    VkDescriptorSetLayoutCreateInfo binding_layout_ci {};
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_ci {};
    std::array<VkDescriptorSetLayoutBinding, 1> bindings {};
    std::array<VkDescriptorBindingFlags, 1> binding_flags {};
    binding_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    binding_layout_ci.pBindings = bindings.data();
    binding_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    binding_flags_ci.pBindingFlags = binding_flags.data();

    binding_layout_ci.bindingCount = 0;
    VK_DEMAND(vkCreateDescriptorSetLayout(DisplayHost::device(), &binding_layout_ci, nullptr, &m_descriptor_layouts[0]));

    binding_layout_ci.bindingCount = 1;
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VK_DEMAND(vkCreateDescriptorSetLayout(DisplayHost::device(), &binding_layout_ci, nullptr, &m_descriptor_layouts[1]));

    binding_layout_ci.pNext = &binding_flags_ci;
    binding_layout_ci.bindingCount = binding_flags_ci.bindingCount = 1;
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = PICTUREBOOK_CAPACITY;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    binding_flags[0] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VK_DEMAND(vkCreateDescriptorSetLayout(DisplayHost::device(), &binding_layout_ci, nullptr, &m_descriptor_layouts[2]));

    std::array<VkDescriptorSetLayout, 3> set_layouts;
    VkPipelineLayoutCreateInfo pipeline_layout_ci {};
    VkPushConstantRange push_constant_range {};
    pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_ci.pSetLayouts = set_layouts.data();
    pipeline_layout_ci.pushConstantRangeCount = 1;
    pipeline_layout_ci.pPushConstantRanges = &push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_ALL;
    push_constant_range.offset = 0;
    push_constant_range.size = 3 * sizeof(uint64_t);

    pipeline_layout_ci.setLayoutCount = 3;
    set_layouts[0] = m_descriptor_layouts[1];
    set_layouts[1] = m_descriptor_layouts[0];
    set_layouts[2] = m_descriptor_layouts[2];
    VK_DEMAND(vkCreatePipelineLayout(DisplayHost::device(), &pipeline_layout_ci, nullptr, &m_graphics_pipeline_layouts[static_cast<size_t>(GraphicsPipeline::GPass)]));

    std::array<VkDescriptorPoolSize, 1> pool_sizes {};
    VkDescriptorPoolCreateInfo descriptor_pool_ci {};
    descriptor_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_ci.maxSets = 2 + 2 * static_cast<size_t>(GraphicsPipeline::MAX_VALUE);
    descriptor_pool_ci.poolSizeCount = pool_sizes.size();
    descriptor_pool_ci.pPoolSizes = pool_sizes.data();
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    VK_DEMAND(vkCreateDescriptorPool(DisplayHost::device(), &descriptor_pool_ci, nullptr, &m_graphics_descriptor_pool));

    VkDescriptorSetAllocateInfo descriptor_alloc_info {};
    auto m_descriptor_set_1_layouts = std::to_array<VkDescriptorSetLayout>({
        m_descriptor_layouts[0],
    });
    descriptor_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_alloc_info.descriptorPool = m_graphics_descriptor_pool;
    descriptor_alloc_info.descriptorSetCount = 1;
    descriptor_alloc_info.pSetLayouts = &m_descriptor_layouts[1];
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, &m_descriptor_set_0[0]));
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, &m_descriptor_set_0[1]));
    descriptor_alloc_info.descriptorSetCount = m_descriptor_set_1_layouts.size();
    descriptor_alloc_info.pSetLayouts = m_descriptor_set_1_layouts.data();
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, m_descriptor_set_1[0].data()));
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, m_descriptor_set_1[1].data()));

    std::array<VkWriteDescriptorSet, 2> descriptor_writes {};
    std::array<VkDescriptorBufferInfo, 2> descriptor_buffer_writes {};
    descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[0].dstSet = m_descriptor_set_0[0];
    descriptor_writes[0].dstBinding = 0;
    descriptor_writes[0].descriptorCount = 1;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[0].pBufferInfo = &descriptor_buffer_writes[0];
    descriptor_buffer_writes[0].buffer = m_uniform_buffer;
    descriptor_buffer_writes[0].offset = 0;
    descriptor_buffer_writes[0].range = 2 * sizeof(mat4);
    descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[1].dstSet = m_descriptor_set_0[1];
    descriptor_writes[1].dstBinding = 0;
    descriptor_writes[1].descriptorCount = 1;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[1].pBufferInfo = &descriptor_buffer_writes[1];
    descriptor_buffer_writes[1].buffer = m_uniform_buffer;
    descriptor_buffer_writes[1].offset = 2 * sizeof(mat4);
    descriptor_buffer_writes[1].range = 2 * sizeof(mat4);
    vkUpdateDescriptorSets(DisplayHost::device(), descriptor_writes.size(), descriptor_writes.data(), 0, nullptr);

    resize_frames(DisplayHost::swapchain_extent());
}

IRenderer::~IRenderer()
{
    std::set<VkPipeline> unique_pipelines;
    std::set<VkPipelineLayout> unique_pipeline_layouts;
    unique_pipelines.insert(m_graphics_pipelines.begin(), m_graphics_pipelines.end());
    unique_pipelines.insert(m_compute_pipelines.begin(), m_compute_pipelines.end());
    unique_pipeline_layouts.insert(m_graphics_pipeline_layouts.begin(), m_graphics_pipeline_layouts.end());
    unique_pipeline_layouts.insert(m_compute_pipeline_layouts.begin(), m_compute_pipeline_layouts.end());

    for (auto it = unique_pipelines.begin(); it != unique_pipelines.end(); ++it)
        vkDestroyPipeline(DisplayHost::device(), *it, nullptr);
    for (auto it = unique_pipeline_layouts.begin(); it != unique_pipeline_layouts.end(); ++it)
        vkDestroyPipelineLayout(DisplayHost::device(), *it, nullptr);
    vkDestroyDescriptorPool(DisplayHost::device(), m_graphics_descriptor_pool, nullptr);
    for (auto it = m_descriptor_layouts.begin(); it != m_descriptor_layouts.end(); ++it)
        vkDestroyDescriptorSetLayout(DisplayHost::device(), *it, nullptr);
    vkDestroyRenderPass(DisplayHost::device(), m_render_pass, nullptr);
    vkDestroySampler(DisplayHost::device(), m_sampler, nullptr);
    vmaDestroyBuffer(DisplayHost::allocator(), m_uniform_buffer, m_uniform_buffer_mem);
}

std::span<std::byte> IRenderer::descriptor_buffer(int frame, int set, int binding)
{
    frame %= SIMULTANEOUS_FRAMES;

    if (set == 0 && binding == 0) {
        if (frame == 0)
            return std::span(m_uniform_buffer_ptr + 0, 2 * sizeof(mat4));
        if (frame == 1)
            return std::span(m_uniform_buffer_ptr + 2 * sizeof(mat4), 2 * sizeof(mat4));
    }
    return {};
}

void IRenderer::flush_descriptor_buffers()
{
    vmaFlushAllocation(DisplayHost::allocator(), m_uniform_buffer_mem, 0, VK_WHOLE_SIZE);
}

void IRenderer::bind_pipeline(VkCommandBuffer cmd, GraphicsPipeline pass, int frame_number)
{
    std::array<VkDescriptorSet, 2> sets = { m_descriptor_set_0[frame_number % SIMULTANEOUS_FRAMES], m_descriptor_set_1[frame_number % SIMULTANEOUS_FRAMES][static_cast<size_t>(pass)] };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipelines[static_cast<size_t>(pass)]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline_layouts[static_cast<size_t>(pass)], 0, sets.size(), sets.data(), 0, nullptr);
}

void IRenderer::resize_frames(VkExtent2D surface_extent)
{
    constexpr float vertical_fov = 70.0f * M_PI / 180.0f;
    const float cot_vertical_fov = 1.f / SDL_tanf(0.5f * vertical_fov);
    m_perspective_projection.m00 = cot_vertical_fov * surface_extent.height / surface_extent.width;
    m_perspective_projection.m11 = -cot_vertical_fov;
    m_perspective_projection.m22 = 0.0f; // infinite far plane
    m_perspective_projection.m23 = -1.0f; // Right-handed look at -Z
    m_perspective_projection.m32 = 0.1f; // near Z plane

    m_ortho_projection.m00 = 2.f / surface_extent.width;
    m_ortho_projection.m11 = -2.f / surface_extent.height;
    m_ortho_projection.m30 = -1.f;
    m_ortho_projection.m31 = 1.f;
    m_ortho_projection.m32 = 1.f;
    m_ortho_projection.m33 = 1.f;
}

SimpleForwardRenderer::SimpleForwardRenderer()
{
    vkGetDeviceQueue(DisplayHost::device(), DisplayHost::queue_family_index(), 0, &m_graphics_queue);

    std::apply([](auto&... subpasses) {
        (memset(&subpasses, 0, sizeof(subpasses)), ...);
    },
        m_pass_discard);
    create_graphics_pipeline();
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        memset(&it->ctx, 0, sizeof(FrameContext));
        create_frame_data(*it);
    }
}

SimpleForwardRenderer::~SimpleForwardRenderer()
{
    vkDeviceWaitIdle(DisplayHost::device());

    destroy_subpass_data(m_pass_discard);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        destroy_subpass_data(it->pass);
        vkDestroySemaphore(DisplayHost::device(), it->ctx.ready, nullptr);
        vkDestroyCommandPool(DisplayHost::device(), it->ctx.command_pool, nullptr);
    }
}

void SimpleForwardRenderer::create_graphics_pipeline()
{
    VkRenderPassCreateInfo2 render_pass_ci {};
    std::array<VkAttachmentDescription2, 2> attachments {};
    std::array<VkSubpassDescription2, 1> subpasses {};
    std::array<VkAttachmentReference2, 1> p0_color_atts = {};
    VkAttachmentReference2 p0_depth_att {};
    render_pass_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    render_pass_ci.attachmentCount = attachments.size();
    render_pass_ci.pAttachments = attachments.data();
    render_pass_ci.subpassCount = subpasses.size();
    render_pass_ci.pSubpasses = subpasses.data();
    attachments[0].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[0].format = DisplayHost::swapchain_format();
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[1].format = DisplayHost::DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpasses[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = p0_color_atts.size();
    subpasses[0].pColorAttachments = p0_color_atts.data();
    subpasses[0].pDepthStencilAttachment = &p0_depth_att;
    p0_color_atts[0].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    p0_color_atts[0].attachment = 0;
    p0_color_atts[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    p0_depth_att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    p0_depth_att.attachment = 1;
    p0_depth_att.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VK_DEMAND(vkCreateRenderPass2(DisplayHost::device(), &render_pass_ci, nullptr, &m_render_pass));

    std::array<VkShaderModule, 2> shader_modules;
    VkShaderModuleCreateInfo shader_module_info {};
    shader_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_module_info.codeSize = shaders::basic_vert_size;
    shader_module_info.pCode = shaders::basic_vert_spv;
    VK_DEMAND(vkCreateShaderModule(DisplayHost::device(), &shader_module_info, nullptr, &shader_modules[0]));
    shader_module_info.codeSize = shaders::basic_frag_size;
    shader_module_info.pCode = shaders::basic_frag_spv;
    VK_DEMAND(vkCreateShaderModule(DisplayHost::device(), &shader_module_info, nullptr, &shader_modules[1]));

    std::array<VkPipelineShaderStageCreateInfo, 2> pipeline_shaders {};
    pipeline_shaders[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    pipeline_shaders[0].module = shader_modules[0];
    pipeline_shaders[0].pName = "main";
    pipeline_shaders[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    pipeline_shaders[1].module = shader_modules[1];
    pipeline_shaders[1].pName = "main";

    // Each attribute (position, normal, tangent, uv, color) has its own binding.
    // Within each binding, the attributes are interleaved.
    // So, if there is a vec2 uv0 and uv1, the stride is 16, and uv1's offset is 8.
    std::array<VkVertexInputBindingDescription, 3> vertex_input_bindings {};
    std::array<VkVertexInputAttributeDescription, 3> vertex_input_atts {};
    vertex_input_bindings[0].binding = 0; // TODO: this value should come from an enum
    vertex_input_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_input_bindings[0].stride = 12;
    vertex_input_bindings[1].binding = 1;
    vertex_input_bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_input_bindings[1].stride = 12;
    vertex_input_bindings[2].binding = 3;
    vertex_input_bindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_input_bindings[2].stride = 8;
    vertex_input_atts[0].location = 0;
    vertex_input_atts[0].binding = 0;
    vertex_input_atts[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_atts[0].offset = 0;
    vertex_input_atts[1].location = 1;
    vertex_input_atts[1].binding = 1;
    vertex_input_atts[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_input_atts[1].offset = 0;
    vertex_input_atts[2].location = 2;
    vertex_input_atts[2].binding = 3;
    vertex_input_atts[2].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_input_atts[2].offset = 0;

    std::array<VkPipelineVertexInputStateCreateInfo, 1> vertex_input_info {};
    vertex_input_info[0].sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info[0].vertexBindingDescriptionCount = 3;
    vertex_input_info[0].pVertexBindingDescriptions = &vertex_input_bindings[0];
    vertex_input_info[0].vertexAttributeDescriptionCount = 3;
    vertex_input_info[0].pVertexAttributeDescriptions = &vertex_input_atts[0];

    VkPipelineInputAssemblyStateCreateInfo input_assy_info {};
    input_assy_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assy_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_info {};
    viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_info.viewportCount = 1;
    viewport_info.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer_info {};
    rasterizer_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer_info.depthClampEnable = VK_FALSE;
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
    depth_stencil_info.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
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
    auto dynamic_state_set = std::to_array<VkDynamicState>({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE });
    dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_info.dynamicStateCount = dynamic_state_set.size();
    dynamic_state_info.pDynamicStates = dynamic_state_set.data();

    std::array<VkGraphicsPipelineCreateInfo, static_cast<size_t>(GraphicsPipeline::MAX_VALUE)> graphics_pipeline_ci {};
    graphics_pipeline_ci[0].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    graphics_pipeline_ci[0].stageCount = 2;
    graphics_pipeline_ci[0].pStages = &pipeline_shaders[0];
    graphics_pipeline_ci[0].pVertexInputState = &vertex_input_info[0];
    graphics_pipeline_ci[0].pInputAssemblyState = &input_assy_info;
    graphics_pipeline_ci[0].pViewportState = &viewport_info;
    graphics_pipeline_ci[0].pRasterizationState = &rasterizer_info;
    graphics_pipeline_ci[0].pMultisampleState = &multisample_info;
    graphics_pipeline_ci[0].pDepthStencilState = &depth_stencil_info;
    graphics_pipeline_ci[0].pColorBlendState = &color_blend_info;
    graphics_pipeline_ci[0].pDynamicState = &dynamic_state_info;
    graphics_pipeline_ci[0].layout = m_graphics_pipeline_layouts[0];
    graphics_pipeline_ci[0].renderPass = m_render_pass;
    graphics_pipeline_ci[0].subpass = 0;
    VK_DEMAND(vkCreateGraphicsPipelines(DisplayHost::device(), DisplayHost::pipeline_cache(), graphics_pipeline_ci.size(), graphics_pipeline_ci.data(), nullptr, m_graphics_pipelines.data()));

    std::array<VkComputePipelineCreateInfo, static_cast<size_t>(ComputePipeline::MAX_VALUE)> compute_pipeline_ci {};
    if constexpr (compute_pipeline_ci.empty() == false)
        VK_DEMAND(vkCreateComputePipelines(DisplayHost::device(), DisplayHost::pipeline_cache(), compute_pipeline_ci.size(), compute_pipeline_ci.data(), nullptr, m_compute_pipelines.data()));

    for (auto it = shader_modules.begin(); it != shader_modules.end(); ++it)
        vkDestroyShaderModule(DisplayHost::device(), *it, nullptr);
}

void SimpleForwardRenderer::create_frame_data(FrameData& frame)
{
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = DisplayHost::queue_family_index();
    VK_DEMAND(vkCreateCommandPool(DisplayHost::device(), &pool_info, nullptr, &frame.ctx.command_pool));

    VkCommandBufferAllocateInfo allocinfo {};
    allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocinfo.commandPool = frame.ctx.command_pool;
    allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocinfo.commandBufferCount = 1;
    VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::device(), &allocinfo, &frame.ctx.command_container));

    VkSemaphoreCreateInfo sem_info {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_DEMAND(vkCreateSemaphore(DisplayHost::device(), &sem_info, nullptr, &frame.ctx.ready));

    create_subpass_data(frame.pass);
}

void SimpleForwardRenderer::create_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<GPass>(subpasses);
        VmaAllocationCreateInfo mem_createinfo {};
        VkImageCreateInfo i_createinfo {};
        VkImageViewCreateInfo iv_createinfo {};
        VkFramebufferCreateInfo fb_createinfo {};
        std::array<VkImageView, 2> fb_attachments;

        fb_createinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_createinfo.renderPass = m_render_pass;
        fb_createinfo.attachmentCount = fb_attachments.size();
        fb_createinfo.pAttachments = fb_attachments.data();
        fb_createinfo.width = DisplayHost::swapchain_extent().width;
        fb_createinfo.height = DisplayHost::swapchain_extent().height;
        fb_createinfo.layers = 1;

        i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        i_createinfo.imageType = VK_IMAGE_TYPE_2D;
        i_createinfo.format = DisplayHost::swapchain_format();
        i_createinfo.extent.width = DisplayHost::swapchain_extent().width;
        i_createinfo.extent.height = DisplayHost::swapchain_extent().height;
        i_createinfo.extent.depth = 1;
        i_createinfo.mipLevels = 1;
        i_createinfo.arrayLayers = 1;
        i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
        i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        mem_createinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.color_buffer, &pass.color_buffer_mem, nullptr));
        iv_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_createinfo.subresourceRange.baseMipLevel = 0;
        iv_createinfo.subresourceRange.levelCount = 1;
        iv_createinfo.subresourceRange.baseArrayLayer = 0;
        iv_createinfo.subresourceRange.layerCount = 1;
        iv_createinfo.image = pass.color_buffer;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.color_buffer_view));

        i_createinfo.format = DisplayHost::DEPTH_FORMAT;
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.depth_buffer, &pass.depth_buffer_mem, nullptr));
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        iv_createinfo.image = pass.depth_buffer;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.depth_buffer_view));

        fb_attachments = { pass.color_buffer_view, pass.depth_buffer_view };
        VK_DEMAND(vkCreateFramebuffer(DisplayHost::device(), &fb_createinfo, nullptr, &pass.framebuffer));
    }
}

void SimpleForwardRenderer::destroy_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<GPass>(subpasses);
        vkDestroyFramebuffer(DisplayHost::device(), pass.framebuffer, nullptr);
        vkDestroyImageView(DisplayHost::device(), pass.depth_buffer_view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.depth_buffer, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.depth_buffer_mem);
        vkDestroyImageView(DisplayHost::device(), pass.color_buffer_view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.color_buffer, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.color_buffer_mem);
    }
}

IRenderer::Output SimpleForwardRenderer::draw(uint32_t frame_number)
{
    const FrameData& frame = m_frame_data[frame_number % m_frame_data.size()];
    vkResetCommandPool(DisplayHost::device(), frame.ctx.command_pool, 0);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(frame.ctx.command_container, &begin_info));

    VkRenderPassBeginInfo render_pass_begin {};
    std::array<VkClearValue, 2> clear_values = { { { { { 0.9375f, 0.6953125f, 0.734375f, 1.0f } } }, { { { 0.0f, 0.0f } } } } };
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = m_render_pass;
    render_pass_begin.framebuffer = std::get<0>(frame.pass).framebuffer;
    render_pass_begin.renderArea.offset = { 0, 0 };
    render_pass_begin.renderArea.extent = DisplayHost::swapchain_extent();
    render_pass_begin.clearValueCount = clear_values.size();
    render_pass_begin.pClearValues = clear_values.data();
    SceneHost::wait_frame(frame_number);

    for (size_t i = 0; i < std::tuple_size<AllSubpasses>::value; i++) {
        if (i == 0)
            vkCmdBeginRenderPass(frame.ctx.command_container, &render_pass_begin, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        else
            vkCmdNextSubpass(frame.ctx.command_container, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        SceneHost::execute_draws(frame.ctx.command_container, frame_number, i);
    }
    vkCmdEndRenderPass(frame.ctx.command_container);
    VK_DEMAND(vkEndCommandBuffer(frame.ctx.command_container));

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 0;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.ctx.command_container;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.ctx.ready;
    VK_DEMAND(vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE));
    return IRenderer::Output(std::get<0>(frame.pass).color_buffer, frame.ctx.ready);
}

void SimpleForwardRenderer::recreate_subpass_data(uint32_t frame_number)
{
    destroy_subpass_data(m_pass_discard);
    destroy_subpass_data(m_frame_data[frame_number % 2].pass);

    std::swap(m_pass_discard, m_frame_data[(frame_number + 1) % 2].pass);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        create_subpass_data(it->pass);
    }
}

}
