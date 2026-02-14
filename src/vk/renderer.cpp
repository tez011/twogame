#include "display.h"
#include "embedded_shaders.h"
#include "scene.h"

namespace twogame {

IRenderer::IRenderer()
    : m_render_pass(VK_NULL_HANDLE)
{
}

IRenderer::~IRenderer()
{
    m_pipelines.clear();
    vkDestroyRenderPass(DisplayHost::instance().device(), m_render_pass, nullptr);
}

SimpleForwardRenderer::SimpleForwardRenderer()
{
    vkGetDeviceQueue(DisplayHost::instance().device(), DisplayHost::instance().queue_family_index(QueueType::Graphics), 0, &m_graphics_queue);

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
    vkDeviceWaitIdle(DisplayHost::instance().device());

    destroy_subpass_data(m_pass_discard);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        destroy_subpass_data(it->pass);
        vkDestroySemaphore(DisplayHost::instance().device(), it->ctx.ready, nullptr);
        vkDestroyCommandPool(DisplayHost::instance().device(), it->ctx.command_pool, nullptr);
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
    attachments[0].format = DisplayHost::instance().swapchain_format();
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[1].format = DisplayHost::instance().depth_format();
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
    VK_DEMAND(vkCreateRenderPass2(DisplayHost::instance().device(), &render_pass_ci, nullptr, &m_render_pass));

    PipelineBuilder pipeline_builder;
    m_pipelines.push_back(pipeline_builder.new_graphics(m_render_pass, 0)
                              .with_shader(twogame::shaders::basic_vert_spv, twogame::shaders::basic_vert_size, VK_SHADER_STAGE_VERTEX_BIT)
                              .with_shader(twogame::shaders::basic_frag_spv, twogame::shaders::basic_frag_size, VK_SHADER_STAGE_FRAGMENT_BIT)
                              .build());
}

void SimpleForwardRenderer::create_frame_data(FrameData& frame)
{
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = DisplayHost::instance().queue_family_index(QueueType::Graphics);
    VK_DEMAND(vkCreateCommandPool(DisplayHost::instance().device(), &pool_info, nullptr, &frame.ctx.command_pool));

    VkCommandBufferAllocateInfo allocinfo {};
    allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocinfo.commandPool = frame.ctx.command_pool;
    allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocinfo.commandBufferCount = 1;
    VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::instance().device(), &allocinfo, &frame.ctx.command_container));

    VkSemaphoreCreateInfo sem_info {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_DEMAND(vkCreateSemaphore(DisplayHost::instance().device(), &sem_info, nullptr, &frame.ctx.ready));

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
        fb_createinfo.width = DisplayHost::instance().swapchain_extent().width;
        fb_createinfo.height = DisplayHost::instance().swapchain_extent().height;
        fb_createinfo.layers = 1;

        i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        i_createinfo.imageType = VK_IMAGE_TYPE_2D;
        i_createinfo.format = DisplayHost::instance().swapchain_format();
        i_createinfo.extent.width = DisplayHost::instance().swapchain_extent().width;
        i_createinfo.extent.height = DisplayHost::instance().swapchain_extent().height;
        i_createinfo.extent.depth = 1;
        i_createinfo.mipLevels = 1;
        i_createinfo.arrayLayers = 1;
        i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
        i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        mem_createinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::instance().allocator(), &i_createinfo, &mem_createinfo, &pass.color_buffer, &pass.color_buffer_mem, nullptr));
        iv_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_createinfo.subresourceRange.baseMipLevel = 0;
        iv_createinfo.subresourceRange.levelCount = 1;
        iv_createinfo.subresourceRange.baseArrayLayer = 0;
        iv_createinfo.subresourceRange.layerCount = 1;
        iv_createinfo.image = pass.color_buffer;
        VK_DEMAND(vkCreateImageView(DisplayHost::instance().device(), &iv_createinfo, nullptr, &pass.color_buffer_view));

        i_createinfo.format = DisplayHost::instance().depth_format();
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::instance().allocator(), &i_createinfo, &mem_createinfo, &pass.depth_buffer, &pass.depth_buffer_mem, nullptr));
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        iv_createinfo.image = pass.depth_buffer;
        VK_DEMAND(vkCreateImageView(DisplayHost::instance().device(), &iv_createinfo, nullptr, &pass.depth_buffer_view));

        fb_attachments = { pass.color_buffer_view, pass.depth_buffer_view };
        VK_DEMAND(vkCreateFramebuffer(DisplayHost::instance().device(), &fb_createinfo, nullptr, &pass.framebuffer));
    }
}

void SimpleForwardRenderer::destroy_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<Subpass0>(subpasses);
        vkDestroyFramebuffer(DisplayHost::instance().device(), pass.framebuffer, nullptr);
        vkDestroyImageView(DisplayHost::instance().device(), pass.depth_buffer_view, nullptr);
        vkDestroyImage(DisplayHost::instance().device(), pass.depth_buffer, nullptr);
        vmaFreeMemory(DisplayHost::instance().allocator(), pass.depth_buffer_mem);
        vkDestroyImageView(DisplayHost::instance().device(), pass.color_buffer_view, nullptr);
        vkDestroyImage(DisplayHost::instance().device(), pass.color_buffer, nullptr);
        vmaFreeMemory(DisplayHost::instance().allocator(), pass.color_buffer_mem);
    }
}

IRenderer::Output SimpleForwardRenderer::draw(SceneHost* stage, uint32_t frame_number)
{
    const FrameData& frame = m_frame_data[frame_number % m_frame_data.size()];
    vkResetCommandPool(DisplayHost::instance().device(), frame.ctx.command_pool, 0);

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
    render_pass_begin.renderArea.extent = DisplayHost::instance().swapchain_extent();
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
