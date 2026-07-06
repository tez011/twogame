#include "staging_buffer.h"
#include "core/debug.h"
#include "display_host.h"

namespace twogame {

void StagingBuffer::advance(size_t offset)
{
    m_tail += offset;
    SDL_assert(m_tail <= size());
}

void StagingBuffer::copy_buffer(VkBuffer dst, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkDeviceSize min_offset = regions.begin()->dstOffset, max_offset = min_offset;
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        min_offset = std::min(it->dstOffset, min_offset);
        max_offset = std::max(it->dstOffset + it->size, max_offset);
    }

    VkBufferMemoryBarrier2& barrier = m_buffer_memory_barriers.emplace_back();
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = DisplayHost::queue_family_index_dma();
    barrier.dstQueueFamilyIndex = DisplayHost::queue_family_index();
    barrier.buffer = dst;
    barrier.offset = min_offset;
    barrier.size = max_offset - min_offset;

    auto& copy = m_buffer_copies.emplace_back();
    copy.second = std::vector(regions.begin(), regions.end());
    copy.first.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copy.first.srcBuffer = m_src_buffer;
    copy.first.dstBuffer = dst;
    copy.first.regionCount = copy.second.size();
    copy.first.pRegions = copy.second.data();
}

void StagingBuffer::copy_image(VkImage dst, const VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout)
{
    VkImageMemoryBarrier2 barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dst;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = info.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = info.arrayLayers;
    m_image_memory_barriers[0].push_back(barrier);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.newLayout = final_layout;
    barrier.srcQueueFamilyIndex = DisplayHost::queue_family_index_dma();
    barrier.dstQueueFamilyIndex = DisplayHost::queue_family_index();
    m_image_memory_barriers[1].push_back(barrier);

    auto& copy = m_image_copies.emplace_back();
    copy.second = std::vector(copies.begin(), copies.end());
    copy.first.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    copy.first.srcBuffer = m_src_buffer;
    copy.first.dstImage = dst;
    copy.first.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy.first.regionCount = copy.second.size();
    copy.first.pRegions = copy.second.data();
}

void StagingBuffer::finalize()
{
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(m_xfer_commands, &begin_info));
    vmaFlushAllocation(DisplayHost::allocator(), m_src_mem, 0, VK_WHOLE_SIZE);

    VkDependencyInfo dep {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = m_image_memory_barriers[0].size();
    dep.pImageMemoryBarriers = m_image_memory_barriers[0].data();
    vkCmdPipelineBarrier2(m_xfer_commands, &dep);
    for (auto it = m_buffer_copies.begin(); it != m_buffer_copies.end(); ++it)
        vkCmdCopyBuffer2(m_xfer_commands, &it->first);
    for (auto it = m_image_copies.begin(); it != m_image_copies.end(); ++it)
        vkCmdCopyBufferToImage2(m_xfer_commands, &it->first);

    dep.bufferMemoryBarrierCount = m_buffer_memory_barriers.size();
    dep.pBufferMemoryBarriers = m_buffer_memory_barriers.data();
    dep.imageMemoryBarrierCount = m_image_memory_barriers[1].size();
    dep.pImageMemoryBarriers = m_image_memory_barriers[1].data();
    vkCmdPipelineBarrier2(m_xfer_commands, &dep);
    VK_DEMAND(vkEndCommandBuffer(m_xfer_commands));

    if (m_acquire_commands != VK_NULL_HANDLE) {
        VK_DEMAND(vkBeginCommandBuffer(m_acquire_commands, &begin_info));
        vkCmdPipelineBarrier2(m_acquire_commands, &dep);
        VK_DEMAND(vkEndCommandBuffer(m_acquire_commands));
    }

    m_buffer_copies.clear();
    m_image_copies.clear();
    m_buffer_memory_barriers.clear();
    m_image_memory_barriers[0].clear();
    m_image_memory_barriers[1].clear();

    // This is ok to do here, but only because we don't touch the staging buffer between this function
    // and when the command buffers are done executing.
    m_tail = 0;
}

}
