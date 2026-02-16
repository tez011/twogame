#include "display.h"
#include "embedded_shaders.h"
#include "scene.h"

namespace twogame {

IRenderer::IRenderer()
    : m_perspective_projection(GLMS_MAT4_ZERO_INIT)
    , m_ortho_projection(GLMS_MAT4_ZERO_INIT)
    , m_render_pass(VK_NULL_HANDLE)
{
    VkPhysicalDeviceProperties hwd_props;
    vkGetPhysicalDeviceProperties(DisplayHost::hardware_device(), &hwd_props);

    VkSamplerCreateInfo sampler_info {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = hwd_props.limits.maxSamplerAnisotropy;
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_DEMAND(vkCreateSampler(DisplayHost::device(), &sampler_info, nullptr, &m_sampler));

    resize_frames(DisplayHost::swapchain_extent());
}

IRenderer::~IRenderer()
{
    m_pipelines.clear();
    vkDestroyRenderPass(DisplayHost::device(), m_render_pass, nullptr);
    vkDestroySampler(DisplayHost::device(), m_sampler, nullptr);
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
        auto& pass = std::get<Subpass0>(subpasses);
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
