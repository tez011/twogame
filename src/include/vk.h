#pragma once
#include "render.h"
#include <array>
#include <mutex>
#include <optional>
#include <queue>
#include <SDL.h>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#ifdef TWOGAME_DEBUG_BUILD
#define VK_CHECK(X) VK_CHECK_3(X, __FILE__, __LINE__)
#define VK_CHECK_3(X, F, L)                                                                               \
    do {                                                                                                  \
        VkResult __bc_res;                                                                                \
        if ((__bc_res = (X)) != VK_SUCCESS) {                                                             \
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "assert at " F ":%d: " #X ": %d", L, __bc_res); \
            std::terminate();                                                                             \
        }                                                                                                 \
    } while (0)
#else
#define VK_BLIND_CHECK(X)        \
    do {                         \
        if ((X) != VK_SUCCESS) { \
            std::terminate();    \
        }                        \
    } while (0)
#endif

namespace twogame {

namespace vk_destructible {

#define DESTRUCTIBLE_TYPES \
    X(VkSwapchainKHR)      \
    X(VkBuffer)            \
    X(VkImage)             \
    X(VkImageView)         \
    X(VkRenderPass)        \
    X(VkFramebuffer)       \
    X(VkSampler)           \
    X(VmaAllocation)

    enum class Types {
#define X(T) D##T,
        DESTRUCTIBLE_TYPES
#undef X
    };

    template <typename T>
    struct Type { };
#define X(T)                                               \
    template <>                                            \
    struct Type<T> {                                       \
        static constexpr Types t() { return Types::D##T; } \
    };
    DESTRUCTIBLE_TYPES
#undef X

}

class VulkanRenderer : public Renderer {
private:
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_2;
    static PFN_vkDestroyDebugUtilsMessengerEXT s_vkDestroyDebugUtilsMessenger;

    enum class Queues {
        Universal,
        Secondary,
        Compute,
        Transfer,
        MAX_VALUE,
    };
    enum class RenderAttachments {
        ColorBuffer,
        DepthBuffer,
        MAX_VALUE,
    };

    VkInstance m_instance;
    VkDebugUtilsMessengerEXT m_debug_messenger;
    VkSurfaceKHR m_surface;
    VkPhysicalDevice m_hwd;
    VkDevice m_device;
    VmaAllocator m_allocator;
    VkPhysicalDeviceLimits m_device_limits {};
    VkPhysicalDeviceFeatures2 m_device_features {};
    VkPhysicalDeviceVulkan11Features m_device_features11 {};
    VkPhysicalDeviceVulkan12Features m_device_features12 {};
    VkSwapchainKHR m_swapchain;
    VkSurfaceFormatKHR m_swapchain_format;
    VkExtent2D m_swapchain_extent;
    VkPipelineCache m_pipeline_cache;
    VkSampler m_active_sampler;
    std::array<VkQueue, static_cast<size_t>(Queues::MAX_VALUE)> m_queues;
    std::array<std::optional<std::mutex>, static_cast<size_t>(Queues::MAX_VALUE)> m_queue_locks;
    uint64_t m_frame_number = 0;
    bool m_mip_filter = false;
    float m_requested_anisotropy = 0;

    std::array<VkRenderPass, 1> m_render_pass;
    std::array<std::array<VkFramebuffer, 1>, 2> m_framebuffers;
    std::array<std::array<VkImage, static_cast<size_t>(RenderAttachments::MAX_VALUE)>, 2> m_render_atts;
    std::array<std::array<VkImageView, static_cast<size_t>(RenderAttachments::MAX_VALUE)>, 2> m_render_att_views;
    std::array<std::array<VmaAllocation, static_cast<size_t>(RenderAttachments::MAX_VALUE)>, 2> m_render_att_allocs;
    std::array<std::queue<std::pair<uint64_t, vk_destructible::Types>>, 4> m_trash;

    void create_instance();
    void create_debug_messenger();
    void pick_physical_device();
    void create_logical_device();
    void create_allocator();
    void create_swapchain(VkSwapchainKHR old_swapchain);
    void create_render_pass();
    void create_framebuffers();
    void create_pipeline_cache();
    void create_sampler();

    void release_freed_items(int bucket);
    void recreate_swapchain();
    void write_pipeline_cache();

    template <typename T, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void vfree(T i)
    {
        m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
    }
    template <typename T, size_t N, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void vfree(std::array<T, N>& c)
    {
        for (const T& i : c)
            m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
        c.fill(VK_NULL_HANDLE);
    }
    template <typename T, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void vfree(std::vector<T>& c)
    {
        for (const T& i : c)
            m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
        c.clear();
    }

public:
    VulkanRenderer(SDL_Window*);
    ~VulkanRenderer();
};

}