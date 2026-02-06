#include "display.h"
#include "embedded_shaders.h"
#include "scene.h"

namespace twogame {

IRenderer::~IRenderer()
{
    for (auto it = m_descriptor_layout.begin(); it != m_descriptor_layout.end(); ++it)
        vkDestroyDescriptorSetLayout(device(), *it, nullptr);
    for (auto it = m_pipeline.begin(); it != m_pipeline.end(); ++it)
        vkDestroyPipeline(device(), *it, nullptr);
    for (auto it = m_pipeline_layout.begin(); it != m_pipeline_layout.end(); ++it)
        vkDestroyPipelineLayout(device(), *it, nullptr);
    vkDestroyRenderPass(device(), m_render_pass, nullptr);
}

SimpleForwardRenderer::SimpleForwardRenderer(DisplayHost* host)
    : IRenderer(host)
{
    vkGetDeviceQueue(device(), queue_family_index(QueueType::Graphics), 0, &m_graphics_queue);

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
    vkDeviceWaitIdle(device());

    destroy_subpass_data(m_pass_discard);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        destroy_subpass_data(it->pass);
        vkDestroySemaphore(device(), it->ctx.ready, nullptr);
        vkDestroyCommandPool(device(), it->ctx.command_pool, nullptr);
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
    attachments[0].format = swapchain_format();
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[1].format = depth_format();
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
    VK_DEMAND(vkCreateRenderPass2(device(), &render_pass_ci, nullptr, &m_render_pass));
    m_descriptor_layout.resize(2);
    m_pipeline_layout.resize(1);
    m_pipeline.resize(1);

    VkShaderModuleCreateInfo shader_ci {};
    VkShaderModule basic_vert_shader, basic_frag_shader;
    shader_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_ci.codeSize = twogame::shaders::basic_vert_size;
    shader_ci.pCode = twogame::shaders::basic_vert_spv;
    VK_DEMAND(vkCreateShaderModule(device(), &shader_ci, nullptr, &basic_vert_shader));
    shader_ci.codeSize = twogame::shaders::basic_frag_size;
    shader_ci.pCode = twogame::shaders::basic_frag_spv;
    VK_DEMAND(vkCreateShaderModule(device(), &shader_ci, nullptr, &basic_frag_shader));

    auto descriptor_bindings = std::to_array<VkDescriptorSetLayoutBinding>({
        // set 0
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        // set 1
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
    });

    auto push_constant_ranges = std::to_array<VkPushConstantRange>({
        { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 },
    });

    VkDescriptorSetLayoutCreateInfo descriptor_set_info {};
    descriptor_set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_set_info.bindingCount = 1;
    descriptor_set_info.pBindings = descriptor_bindings.data() + 0;
    VK_DEMAND(vkCreateDescriptorSetLayout(device(), &descriptor_set_info, nullptr, &m_descriptor_layout[0]));
    descriptor_set_info.pBindings = descriptor_bindings.data() + 1;
    VK_DEMAND(vkCreateDescriptorSetLayout(device(), &descriptor_set_info, nullptr, &m_descriptor_layout[1]));

    VkPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 2;
    pipeline_layout_info.pSetLayouts = m_descriptor_layout.data() + 0;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = push_constant_ranges.data() + 0;
    VK_DEMAND(vkCreatePipelineLayout(device(), &pipeline_layout_info, nullptr, &m_pipeline_layout[0]));

    std::array<VkPipelineShaderStageCreateInfo, 2> shader_info {};
    for (auto it = shader_info.begin(); it != shader_info.end(); ++it) {
        it->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        it->pName = "main";
    }
    shader_info[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_info[0].module = basic_vert_shader;
    shader_info[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_info[1].module = basic_frag_shader;

    VkPipelineDynamicStateCreateInfo dynamic_state_info {};
    auto dynamic_state_set = std::to_array<VkDynamicState>({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR });
    dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_info.dynamicStateCount = dynamic_state_set.size();
    dynamic_state_info.pDynamicStates = dynamic_state_set.data();

    VkPipelineVertexInputStateCreateInfo vertex_input_info {};
    std::array<VkVertexInputBindingDescription, 3> vertex_bindings;
    std::array<VkVertexInputAttributeDescription, 3> vertex_attributes;
    vertex_bindings[0].binding = 0;
    vertex_bindings[0].stride = 3 * sizeof(float);
    vertex_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_bindings[1].binding = 1;
    vertex_bindings[1].stride = 3 * sizeof(float);
    vertex_bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_bindings[2].binding = 2;
    vertex_bindings[2].stride = 2 * sizeof(float);
    vertex_bindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_attributes[0].binding = 0;
    vertex_attributes[0].location = 0;
    vertex_attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_attributes[0].offset = 0;
    vertex_attributes[1].binding = 1;
    vertex_attributes[1].location = 1;
    vertex_attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_attributes[1].offset = 0;
    vertex_attributes[2].binding = 2;
    vertex_attributes[2].location = 2;
    vertex_attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_attributes[2].offset = 0;
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info.vertexBindingDescriptionCount = vertex_bindings.size();
    vertex_input_info.pVertexBindingDescriptions = vertex_bindings.data();
    vertex_input_info.vertexAttributeDescriptionCount = vertex_attributes.size();
    vertex_input_info.pVertexAttributeDescriptions = vertex_attributes.data();

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

    VkGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = shader_info.size();
    pipeline_info.pStages = shader_info.data();
    pipeline_info.pVertexInputState = &vertex_input_info;
    pipeline_info.pInputAssemblyState = &input_assy_info;
    pipeline_info.pViewportState = &viewport_info;
    pipeline_info.pRasterizationState = &rasterizer_info;
    pipeline_info.pMultisampleState = &multisample_info;
    pipeline_info.pDepthStencilState = &depth_stencil_info;
    pipeline_info.pColorBlendState = &color_blend_info;
    pipeline_info.pDynamicState = &dynamic_state_info;
    pipeline_info.layout = m_pipeline_layout[0];
    pipeline_info.renderPass = m_render_pass;
    pipeline_info.subpass = 0;
    VK_DEMAND(vkCreateGraphicsPipelines(device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline[0]));

    vkDestroyShaderModule(device(), basic_vert_shader, nullptr);
    vkDestroyShaderModule(device(), basic_frag_shader, nullptr);
}

void SimpleForwardRenderer::create_frame_data(FrameData& frame)
{
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family_index(QueueType::Graphics);
    VK_DEMAND(vkCreateCommandPool(device(), &pool_info, nullptr, &frame.ctx.command_pool));

    VkCommandBufferAllocateInfo allocinfo {};
    allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocinfo.commandPool = frame.ctx.command_pool;
    allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocinfo.commandBufferCount = 1;
    VK_DEMAND(vkAllocateCommandBuffers(device(), &allocinfo, &frame.ctx.command_container));

    VkSemaphoreCreateInfo sem_info {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_DEMAND(vkCreateSemaphore(device(), &sem_info, nullptr, &frame.ctx.ready));

    create_subpass_data(frame.pass);
}

void SimpleForwardRenderer::create_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<Subpass0>(subpasses);
        VmaAllocationCreateInfo mem_createinfo {};
        VkImageCreateInfo i_createinfo {};
        VkImageViewCreateInfo iv_createinfo {};
        VkFramebufferCreateInfo fb_createinfo {};
        std::array<VkImageView, 2> fb_attachments;

        fb_createinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_createinfo.renderPass = m_render_pass;
        fb_createinfo.attachmentCount = fb_attachments.size();
        fb_createinfo.pAttachments = fb_attachments.data();
        fb_createinfo.width = swapchain_extent().width;
        fb_createinfo.height = swapchain_extent().height;
        fb_createinfo.layers = 1;

        i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        i_createinfo.imageType = VK_IMAGE_TYPE_2D;
        i_createinfo.format = swapchain_format();
        i_createinfo.extent.width = swapchain_extent().width;
        i_createinfo.extent.height = swapchain_extent().height;
        i_createinfo.extent.depth = 1;
        i_createinfo.mipLevels = 1;
        i_createinfo.arrayLayers = 1;
        i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
        i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        mem_createinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_DEMAND(vmaCreateImage(allocator(), &i_createinfo, &mem_createinfo, &pass.color_buffer, &pass.color_buffer_mem, nullptr));
        iv_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_createinfo.subresourceRange.baseMipLevel = 0;
        iv_createinfo.subresourceRange.levelCount = 1;
        iv_createinfo.subresourceRange.baseArrayLayer = 0;
        iv_createinfo.subresourceRange.layerCount = 1;
        iv_createinfo.image = pass.color_buffer;
        VK_DEMAND(vkCreateImageView(device(), &iv_createinfo, nullptr, &pass.color_buffer_view));

        i_createinfo.format = depth_format();
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VK_DEMAND(vmaCreateImage(allocator(), &i_createinfo, &mem_createinfo, &pass.depth_buffer, &pass.depth_buffer_mem, nullptr));
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        iv_createinfo.image = pass.depth_buffer;
        VK_DEMAND(vkCreateImageView(device(), &iv_createinfo, nullptr, &pass.depth_buffer_view));

        fb_attachments = { pass.color_buffer_view, pass.depth_buffer_view };
        VK_DEMAND(vkCreateFramebuffer(device(), &fb_createinfo, nullptr, &pass.framebuffer));
    }
}

void SimpleForwardRenderer::destroy_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<Subpass0>(subpasses);
        vkDestroyFramebuffer(device(), pass.framebuffer, nullptr);
        vkDestroyImageView(device(), pass.depth_buffer_view, nullptr);
        vkDestroyImage(device(), pass.depth_buffer, nullptr);
        vmaFreeMemory(allocator(), pass.depth_buffer_mem);
        vkDestroyImageView(device(), pass.color_buffer_view, nullptr);
        vkDestroyImage(device(), pass.color_buffer, nullptr);
        vmaFreeMemory(allocator(), pass.color_buffer_mem);
    }
}

IRenderer::Output SimpleForwardRenderer::draw(SceneHost* stage, uint32_t frame_number)
{
    const FrameData& frame = m_frame_data[frame_number % m_frame_data.size()];
    vkResetCommandPool(device(), frame.ctx.command_pool, 0);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(frame.ctx.command_container, &begin_info));

    VkRenderPassBeginInfo render_pass_begin {};
    std::array<VkClearValue, 2> clear_values = { { { { { 0.9375f, 0.6953125f, 0.734375f, 1.0f } } }, { { { 1.0f, 0.0f } } } } };
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = m_render_pass;
    render_pass_begin.framebuffer = std::get<0>(frame.pass).framebuffer;
    render_pass_begin.renderArea.offset = { 0, 0 };
    render_pass_begin.renderArea.extent = swapchain_extent();
    render_pass_begin.clearValueCount = clear_values.size();
    render_pass_begin.pClearValues = clear_values.data();
    stage->wait_frame(frame_number);

    for (size_t i = 0; i < std::tuple_size<AllSubpasses>::value; i++) {
        if (i == 0)
            vkCmdBeginRenderPass(frame.ctx.command_container, &render_pass_begin, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        else
            vkCmdNextSubpass(frame.ctx.command_container, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        stage->execute_draws(frame.ctx.command_container, frame_number, i);
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
