#pragma once
#include <array>
#include <span>
#include <vector>
#include <volk.h>
#include "core/constants.h"
#include "vk_mem_alloc.h"

namespace twogame {

class StagingBuffer {
    friend class SceneHost;

    VkBuffer m_src_buffer;
    VmaAllocation m_src_mem;
    std::span<std::byte> m_src_data;
    VkCommandBuffer m_xfer_commands, m_acquire_commands;
    VkSemaphore m_post_xfer;
    VkDeviceSize m_tail;

    std::vector<VkBufferMemoryBarrier2> m_buffer_memory_barriers;
    std::vector<std::pair<VkCopyBufferInfo2, std::vector<VkBufferCopy2>>> m_buffer_copies;
    std::array<std::vector<VkImageMemoryBarrier2>, 2> m_image_memory_barriers;
    std::vector<std::pair<VkCopyBufferToImageInfo2, std::vector<VkBufferImageCopy2>>> m_image_copies;

public:
    StagingBuffer()
        : m_src_buffer(VK_NULL_HANDLE)
        , m_src_mem(VK_NULL_HANDLE)
        , m_tail(0)
    {
    }
    constexpr inline VkDeviceSize size() const { return Constants::STAGING_BUFFER_SIZE; }
    inline VkDeviceSize tail_offset() const { return m_tail; }
    inline std::byte* tail() { return m_src_data.subspan(m_tail).data(); }
    void advance(size_t);

    void copy_image(VkImage dst, const VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout);
    void copy_buffer(VkBuffer dst, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

    /**
     * @warning It is illegal to do any operations on this object until this staging buffer has been submitted to a queue,
     * and the host has been signaled that that operation is complete.
     */
    void finalize();
};

}
