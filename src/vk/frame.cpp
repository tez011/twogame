#include <glm/gtc/matrix_transform.hpp>
#include "render.h"
#include "scene.h"

namespace twogame {

int32_t Renderer::acquire_image()
{
    VkResult acquire_image_result;
    VkFence fence = m_fence_frame[m_frame_number % 2];
    VkSemaphore sem = m_sem_image_available[m_frame_number % 2];
    vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &fence);

    uint32_t image_index;
    acquire_image_result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &image_index);
    if (acquire_image_result == VK_SUCCESS || acquire_image_result == VK_SUBOPTIMAL_KHR) {
        return image_index;
    } else if (acquire_image_result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return image_index;
    } else {
        return -1;
    }
}

void Renderer::draw(Scene* scene)
{
    VkResult res;
    for (auto it = m_command_pools[m_frame_number % 2].begin(); it != m_command_pools[m_frame_number % 2].end(); ++it) {
        for (auto jt = it->begin(); jt != it->end(); ++jt) {
            if (*jt != VK_NULL_HANDLE)
                vkResetCommandPool(m_device, *jt, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
            // Yes, even the ones that belong to worker threads!
            // There will be CPU fences in place preventing concurrent access here.
        }
    }

    if ((res = vkWaitForFences(m_device, 1, &m_fence_assets_prepared, VK_FALSE, 0)) == VK_SUCCESS) {
        vkResetCommandBuffer(m_cbuf_asset_prepare, 0);
        scene->post_prepare_assets();
        if (scene->prepare_assets(m_cbuf_asset_prepare)) { // scene should cache a list of assets currently being prepared, to be cleaned up in the above call.
            VkSubmitInfo apsub {};
            apsub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            apsub.waitSemaphoreCount = 0;
            apsub.commandBufferCount = 1;
            apsub.pCommandBuffers = &m_cbuf_asset_prepare;
            apsub.signalSemaphoreCount = 0;
            vkResetFences(m_device, 1, &m_fence_assets_prepared);
            VK_CHECK(vkQueueSubmit(m_queues[static_cast<size_t>(QueueFamily::Universal)], 1, &apsub, m_fence_assets_prepared));
        }
    } else if (res != VK_TIMEOUT) {
        std::terminate();
    }

    auto ps1i0 = static_cast<descriptor_storage::uniform_s1i0_t*>(m_ds1_buffers[m_frame_number % 2][0].details.pMappedData);
    ps1i0->view = glm::lookAt(glm::vec3(0.f, 250.f, 400.f), glm::vec3(0.f, 100.f, 0.f), glm::vec3(0.f, 1.f, 0.f));
    ps1i0->proj = m_projection;

    std::array<VkMappedMemoryRange, 1> flush_ranges;
    for (size_t i = 0; i < flush_ranges.size(); i++) {
        flush_ranges[i].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flush_ranges[i].pNext = nullptr;
    }
    flush_ranges[0].memory = m_ds1_buffers[m_frame_number % 2][0].details.deviceMemory;
    flush_ranges[0].offset = m_ds1_buffers[m_frame_number % 2][0].details.offset;
    flush_ranges[0].size = std::max(m_device_limits.nonCoherentAtomSize, m_ds1_buffers[m_frame_number % 2][0].details.size);
    vkFlushMappedMemoryRanges(m_device, flush_ranges.size(), flush_ranges.data());

    VkCommandBuffer cbuf = m_command_buffers[m_frame_number % 2][static_cast<size_t>(CommandBuffer::RenderOneStage)];
    VkCommandBufferBeginInfo cbuf_begin {};
    cbuf_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cbuf, &cbuf_begin));

    VkRenderPassBeginInfo cbuf_render_begin {};
    cbuf_render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    cbuf_render_begin.renderPass = m_render_pass[0];
    cbuf_render_begin.framebuffer = m_framebuffers[m_frame_number % 2][0];
    cbuf_render_begin.renderArea.offset = { 0, 0 };
    cbuf_render_begin.renderArea.extent = m_swapchain_extent;

    VkClearValue clear_color[] = { { { { 0.9375f, 0.6953125f, 0.734375f, 1.0f } } }, { { { 1.0f, 0.0f } } } };
    cbuf_render_begin.clearValueCount = 2;
    cbuf_render_begin.pClearValues = clear_color;

    std::array<VkDescriptorSet, 4> descriptor_sets = {
        m_ds0[m_frame_number % 2],
        m_ds1[m_frame_number % 2][0],
        m_ds2[m_frame_number % 2][0],
        VK_NULL_HANDLE,
    };
    vkCmdBeginRenderPass(cbuf, &cbuf_render_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {};
    VkRect2D scissor {};
    viewport.width = m_swapchain_extent.width;
    viewport.height = m_swapchain_extent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.extent = m_swapchain_extent;
    vkCmdSetViewport(cbuf, 0, 1, &viewport);
    vkCmdSetScissor(cbuf, 0, 1, &scissor);

    scene->draw(cbuf, m_render_pass[0], 0, descriptor_sets);

    vkCmdEndRenderPass(cbuf);
    VK_CHECK(vkEndCommandBuffer(cbuf));

    VkSubmitInfo submit {};
    VkPipelineStageFlags submit_wait_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_sem_image_available[m_frame_number % 2];
    submit.pWaitDstStageMask = &submit_wait_mask;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbuf;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_sem_render_finished[m_frame_number % 2];

    VK_CHECK(vkQueueSubmit(m_queues[static_cast<size_t>(QueueFamily::Universal)], 1, &submit, VK_NULL_HANDLE));
}

void Renderer::next_frame(uint32_t image_index)
{
    VkCommandBuffer cbuf = m_command_buffers[m_frame_number % 2][static_cast<size_t>(CommandBuffer::BlitToSwapchain)];
    VkCommandBufferBeginInfo cbuf_begin {};
    cbuf_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cbuf, &cbuf_begin));

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_swapchain_images[image_index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (m_multisample_count == 1) {
        VkImageBlit blit {};
        blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[0] = blit.srcOffsets[1] = { 0, 0, 0 };
        blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(m_swapchain_extent.width), static_cast<int32_t>(m_swapchain_extent.height), 1 };
        vkCmdBlitImage(cbuf, m_render_atts[m_frame_number % 2][static_cast<size_t>(RenderAttachment::ColorBuffer)], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    } else {
        VkImageResolve resolve {};
        resolve.srcSubresource.aspectMask = resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolve.srcSubresource.layerCount = resolve.dstSubresource.layerCount = 1;
        resolve.srcOffset = resolve.dstOffset = { 0, 0, 0 };
        resolve.extent = { m_swapchain_extent.width, m_swapchain_extent.height, 1 };
        vkCmdResolveImage(cbuf, m_render_atts[m_frame_number % 2][static_cast<size_t>(RenderAttachment::ColorBuffer)], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
    }

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VK_CHECK(vkEndCommandBuffer(cbuf));

    VkSubmitInfo submit {};
    VkPipelineStageFlags submit_wait_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_sem_render_finished[m_frame_number % 2];
    submit.pWaitDstStageMask = &submit_wait_mask;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbuf;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_sem_blit_finished[m_frame_number % 2];
    VK_CHECK(vkQueueSubmit(m_queues[static_cast<size_t>(QueueFamily::Universal)], 1, &submit, m_fence_frame[m_frame_number % 2]));

    VkPresentInfoKHR present {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_sem_blit_finished[m_frame_number % 2];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &image_index;

    VkResult res = vkQueuePresentKHR(m_queues[static_cast<size_t>(QueueFamily::Universal)], &present);
    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
    } else if (res != VK_SUCCESS) {
        abort();
    }

    m_frame_number++;

    // This at the "start of the next frame"
    release_freed_items(m_frame_number % 4);
}

}
