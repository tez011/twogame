#include "shaders.h"
#include "twogame.h"
#include "twogame_vk.h"

namespace twogame::vk {

SimpleForwardRenderer::SimpleForwardRenderer(DisplayHost* host)
    : twogame::vk::IRenderer(host)
{
    create_graphics_pipeline();
    create_framebuffers();

    memset(&m_fb_discard, 0, sizeof(Framebuffers));
    vkGetDeviceQueue(device(), queue_family_index(twogame::vk::DisplayHost::QueueType::Graphics), 0, &m_graphics_queue);
}

SimpleForwardRenderer::~SimpleForwardRenderer()
{
    vkDeviceWaitIdle(device());

    destroy_framebuffer_sized_items(m_fb_discard);
    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end(); ++it) {
        vkDestroySemaphore(device(), it->color_buffer_ready, nullptr);
        vkDestroyCommandPool(device(), it->command_pool, nullptr);
        destroy_framebuffer_sized_items(*it);
    }
    vkDestroyPipeline(device(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(device(), m_pipeline_layout, nullptr);
    vkDestroyRenderPass(device(), m_render_pass, nullptr);
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

    VkShaderModuleCreateInfo shader_ci {};
    VkShaderModule tri_vert_shader, tri_frag_shader;
    shader_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_ci.codeSize = twogame::shaders::tri_vert_size;
    shader_ci.pCode = twogame::shaders::tri_vert_spv;
    VK_DEMAND(vkCreateShaderModule(device(), &shader_ci, nullptr, &tri_vert_shader));
    shader_ci.codeSize = twogame::shaders::tri_frag_size;
    shader_ci.pCode = twogame::shaders::tri_frag_spv;
    VK_DEMAND(vkCreateShaderModule(device(), &shader_ci, nullptr, &tri_frag_shader));

    std::array<VkPipelineShaderStageCreateInfo, 2> shader_info {};
    for (auto it = shader_info.begin(); it != shader_info.end(); ++it) {
        it->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        it->pName = "main";
    }
    shader_info[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_info[0].module = tri_vert_shader;
    shader_info[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_info[1].module = tri_frag_shader;

    VkPipelineDynamicStateCreateInfo dynamic_state_info {};
    auto dynamic_state_set = std::to_array<VkDynamicState>({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR });
    dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_info.dynamicStateCount = dynamic_state_set.size();
    dynamic_state_info.pDynamicStates = dynamic_state_set.data();

    VkPipelineVertexInputStateCreateInfo vertex_input_info {};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rasterizer_info.frontFace = VK_FRONT_FACE_CLOCKWISE;
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

    VkPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VK_DEMAND(vkCreatePipelineLayout(device(), &pipeline_layout_info, nullptr, &m_pipeline_layout));

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
    pipeline_info.layout = m_pipeline_layout;
    pipeline_info.renderPass = m_render_pass;
    pipeline_info.subpass = 0;
    VK_DEMAND(vkCreateGraphicsPipelines(device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline));

    vkDestroyShaderModule(device(), tri_vert_shader, nullptr);
    vkDestroyShaderModule(device(), tri_frag_shader, nullptr);
}

void SimpleForwardRenderer::create_framebuffer_sized_items()
{
    VmaAllocationCreateInfo mem_createinfo {};
    VkImageCreateInfo i_createinfo {};
    VkImageViewCreateInfo iv_createinfo {};
    VkFramebufferCreateInfo fb_createinfo {};
    std::array<VkImageView, 2> fb_attachments;
    i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iv_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    fb_createinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_createinfo.renderPass = m_render_pass;
    fb_createinfo.attachmentCount = fb_attachments.size();
    fb_createinfo.pAttachments = fb_attachments.data();
    fb_createinfo.width = swapchain_extent().width;
    fb_createinfo.height = swapchain_extent().height;
    fb_createinfo.layers = 1;

    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end(); ++it) {
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
        iv_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_createinfo.subresourceRange.baseMipLevel = 0;
        iv_createinfo.subresourceRange.levelCount = 1;
        iv_createinfo.subresourceRange.baseArrayLayer = 0;
        iv_createinfo.subresourceRange.layerCount = 1;
        VK_DEMAND(vmaCreateImage(allocator(), &i_createinfo, &mem_createinfo, &it->color_buffer, &it->color_buffer_mem, nullptr));
        iv_createinfo.image = it->color_buffer;
        VK_DEMAND(vkCreateImageView(device(), &iv_createinfo, nullptr, &it->color_buffer_view));

        i_createinfo.format = depth_format();
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        VK_DEMAND(vmaCreateImage(allocator(), &i_createinfo, &mem_createinfo, &it->depth_buffer, &it->depth_buffer_mem, nullptr));
        iv_createinfo.image = it->depth_buffer;
        VK_DEMAND(vkCreateImageView(device(), &iv_createinfo, nullptr, &it->depth_buffer_view));

        fb_attachments[0] = it->color_buffer_view;
        fb_attachments[1] = it->depth_buffer_view;
        VK_DEMAND(vkCreateFramebuffer(device(), &fb_createinfo, nullptr, &it->framebuffer));
    }
}

void SimpleForwardRenderer::create_framebuffers()
{
    VkCommandPoolCreateInfo pool_createinfo {};
    VkCommandBufferAllocateInfo cbuf_allocinfo {};
    VkSemaphoreCreateInfo sem_createinfo {};
    pool_createinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cbuf_allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    sem_createinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto it = m_framebuffers.begin(); it != m_framebuffers.end(); ++it) {
        pool_createinfo.queueFamilyIndex = queue_family_index(twogame::vk::DisplayHost::QueueType::Graphics);
        VK_DEMAND(vkCreateCommandPool(device(), &pool_createinfo, nullptr, &it->command_pool));
        cbuf_allocinfo.commandPool = it->command_pool;
        cbuf_allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbuf_allocinfo.commandBufferCount = it->command_buffer.size();
        VK_DEMAND(vkAllocateCommandBuffers(device(), &cbuf_allocinfo, it->command_buffer.data()));
        VK_DEMAND(vkCreateSemaphore(device(), &sem_createinfo, nullptr, &it->color_buffer_ready));
    }

    create_framebuffer_sized_items();
}

void SimpleForwardRenderer::destroy_framebuffer_sized_items(struct Framebuffers& fb)
{
    vkDestroyFramebuffer(device(), fb.framebuffer, nullptr);
    vkDestroyImageView(device(), fb.depth_buffer_view, nullptr);
    vkDestroyImage(device(), fb.depth_buffer, nullptr);
    vmaFreeMemory(allocator(), fb.depth_buffer_mem);
    vkDestroyImageView(device(), fb.color_buffer_view, nullptr);
    vkDestroyImage(device(), fb.color_buffer, nullptr);
    vmaFreeMemory(allocator(), fb.color_buffer_mem);
}

IRenderer::Output SimpleForwardRenderer::draw()
{
    const Framebuffers& atts = m_framebuffers[frame_number() % m_framebuffers.size()];
    vkResetCommandPool(device(), atts.command_pool, 0);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(atts.command_buffer[0], &begin_info));

    VkRenderPassBeginInfo render_pass_begin {};
    VkSubpassBeginInfo subpass_begin {};
    VkSubpassEndInfo subpass_end {};
    std::array<VkClearValue, 2> clear_values = { { { { { 0.9375f, 0.6953125f, 0.734375f, 1.0f } } }, { { { 1.0f, 0.0f } } } } };
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = m_render_pass;
    render_pass_begin.framebuffer = atts.framebuffer;
    render_pass_begin.renderArea.offset = { 0, 0 };
    render_pass_begin.renderArea.extent = swapchain_extent();
    render_pass_begin.clearValueCount = clear_values.size();
    render_pass_begin.pClearValues = clear_values.data();
    subpass_begin.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
    subpass_begin.contents = VK_SUBPASS_CONTENTS_INLINE;
    subpass_end.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
    vkCmdBeginRenderPass2(atts.command_buffer[0], &render_pass_begin, &subpass_begin);
    vkCmdBindPipeline(atts.command_buffer[0], VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport {};
    VkRect2D scissor {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = swapchain_extent().width;
    viewport.height = swapchain_extent().height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset = { 0, 0 };
    scissor.extent = swapchain_extent();
    vkCmdSetViewport(atts.command_buffer[0], 0, 1, &viewport);
    vkCmdSetScissor(atts.command_buffer[0], 0, 1, &scissor);

    vkCmdDraw(atts.command_buffer[0], 3, 1, 0, 0);
    vkCmdEndRenderPass2(atts.command_buffer[0], &subpass_end);
    VK_DEMAND(vkEndCommandBuffer(atts.command_buffer[0]));

    // Everything above this line is scene logic. Everything below this line is renderer logic.
    // Synchronize command buffers appropriately.

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 0;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &atts.command_buffer[0];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &atts.color_buffer_ready;
    VK_DEMAND(vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE));
    return IRenderer::Output(atts.color_buffer, atts.color_buffer_ready);
}

void SimpleForwardRenderer::recreate_framebuffers(uint32_t frame_number)
{
    destroy_framebuffer_sized_items(m_fb_discard);
    destroy_framebuffer_sized_items(m_framebuffers[frame_number % 2]);
    m_fb_discard.framebuffer = m_framebuffers[(frame_number + 1) % 2].framebuffer;
    m_fb_discard.depth_buffer_view = m_framebuffers[(frame_number + 1) % 2].depth_buffer_view;
    m_fb_discard.depth_buffer = m_framebuffers[(frame_number + 1) % 2].depth_buffer;
    m_fb_discard.depth_buffer_mem = m_framebuffers[(frame_number + 1) % 2].depth_buffer_mem;
    m_fb_discard.color_buffer_view = m_framebuffers[(frame_number + 1) % 2].color_buffer_view;
    m_fb_discard.color_buffer = m_framebuffers[(frame_number + 1) % 2].color_buffer;
    m_fb_discard.color_buffer_mem = m_framebuffers[(frame_number + 1) % 2].color_buffer_mem;
    create_framebuffer_sized_items();
}

}
