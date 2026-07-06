#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include <SDL3/SDL_video.h>
#include <volk.h>
#include "core/constants.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IRenderer;
class SceneHost;

class DisplayHost final {
    friend class SceneHost;
    static std::unique_ptr<DisplayHost> s_self;
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_3;
    constexpr static auto FRAMES_IN_FLIGHT = Constants::FRAMES_IN_FLIGHT;

    std::atomic_uint32_t m_frame_number = 0;
    SDL_Window* m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_hwd = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
    uint32_t m_queue_family_index, m_dma_queue_family_index;
    VkSwapchainKHR m_swapchain;
    VkExtent2D m_swapchain_extent;
    std::vector<VkImage> m_swapchain_images;
    bool m_swapchain_recreated = false;
    VkFormat m_swapchain_format;
    VkCommandPool m_present_command_pool;

    std::vector<VkSemaphore> m_sem_submit_image;
    std::array<VkSemaphore, FRAMES_IN_FLIGHT> m_sem_acquire_image;
    std::array<VkFence, FRAMES_IN_FLIGHT> m_fence_frame;
    std::array<VkCommandBuffer, FRAMES_IN_FLIGHT> m_present_commands;

    bool create_instance();
    bool create_debug_messenger();
    bool create_surface();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_pipeline_artifacts();
    bool create_swapchain(VkSwapchainKHR old_swapchain);
    bool create_syncobjects();
    bool recreate_swapchain();

    DisplayHost();
    int32_t acquire_image();
    void present_image(uint32_t index, VkImage image, VkSemaphore signal);

public:
    static void init();
    static void drop();
    static DisplayHost& owned()
    {
        return *s_self;
    }
    static const DisplayHost& instance()
    {
        return *s_self;
    }
    ~DisplayHost();
    DisplayHost(DisplayHost&) = delete;
    DisplayHost& operator=(const DisplayHost&) = delete;
    DisplayHost(DisplayHost&&) = delete;
    DisplayHost& operator=(DisplayHost&&) = delete;

    static inline VkDevice device() { return s_self->m_device; }
    static inline VmaAllocator allocator() { return s_self->m_allocator; }
    static inline VkPhysicalDevice hardware_device() { return s_self->m_hwd; }
    static inline VkFormat swapchain_format() { return s_self->m_swapchain_format; }
    static inline VkExtent2D swapchain_extent() { return s_self->m_swapchain_extent; }
    static inline uint32_t queue_family_index() { return s_self->m_queue_family_index; }
    static inline uint32_t queue_family_index_dma() { return s_self->m_dma_queue_family_index; }
    static inline VkPipelineCache pipeline_cache() { return s_self->m_pipeline_cache; }

    int draw_frame(IRenderer*);
};

}
