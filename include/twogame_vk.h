#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <vector>
#include <SDL3/SDL.h>
#include <volk.h>
#include "vk_mem_alloc.h"

#ifdef DEBUG_BUILD
#include <vulkan/vk_enum_string_helper.h>
#define VK_DEMAND(X) VK_DEMAND_3P(X, __FILE__, __LINE__)
#define VK_DEMAND_3P(X, F, L)                                                                                 \
    do {                                                                                                      \
        VkResult __r;                                                                                         \
        if ((__r = (X)) != VK_SUCCESS) {                                                                      \
            SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "assert at %s:%d: %s: %s", F, L, #X, string_VkResult(__r)); \
            std::abort();                                                                                     \
        }                                                                                                     \
    } while (0)
#else
#define VK_DEMAND(X)             \
    do {                         \
        if ((X) != VK_SUCCESS) { \
            std::abort();        \
        }                        \
    } while (0)
#endif
namespace twogame::vk {

class IRenderer;

class DisplayHost final {
    friend class IRenderer;

public:
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_3;
    constexpr static int SIMULTANEOUS_FRAMES = 2;
    enum class QueueType {
        Graphics,
        Compute,
        Transfer,
        MAX_VALUE,
    };

private:
    SDL_Window* m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_hwd = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::array<uint32_t, static_cast<size_t>(QueueType::MAX_VALUE)> m_queue_family_indexes;
    VkSwapchainKHR m_swapchain;
    VkExtent2D m_swapchain_extent;
    std::vector<VkImage> m_swapchain_images;
    bool m_swapchain_recreated = false;
    VkFormat m_swapchain_format, m_depth_format;
    VkCommandPool m_present_command_pool;

    std::vector<VkSemaphore> m_sem_submit_image;
    std::array<VkSemaphore, SIMULTANEOUS_FRAMES> m_sem_acquire_image;
    std::array<VkFence, SIMULTANEOUS_FRAMES> m_fence_frame;
    std::array<VkCommandBuffer, SIMULTANEOUS_FRAMES> m_present_commands;

    uint32_t m_frame_number;

    DisplayHost() { }
    bool create_instance();
    bool create_debug_messenger();
    bool create_surface();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain(VkSwapchainKHR old_swapchain);
    bool create_syncobjects();
    bool recreate_swapchain();

    int32_t acquire_image();
    void present_image(uint32_t index, VkImage image, VkSemaphore signal);

public:
    static DisplayHost* create();
    ~DisplayHost();

    SDL_AppResult handle_event(SDL_Event*);
    SDL_AppResult draw_frame(IRenderer*);
};

class IRenderer {
protected:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;

    twogame::vk::DisplayHost& r_host;

    IRenderer(DisplayHost* host)
        : r_host(*host)
    {
    }

    inline constexpr VkDevice device() const { return r_host.m_device; }
    inline constexpr VmaAllocator allocator() const { return r_host.m_allocator; }
    inline constexpr VkExtent2D swapchain_extent() const { return r_host.m_swapchain_extent; }
    inline constexpr VkFormat swapchain_format() const { return r_host.m_swapchain_format; }
    inline constexpr uint32_t queue_family_index(DisplayHost::QueueType t) { return r_host.m_queue_family_indexes[static_cast<size_t>(t)]; }
    inline constexpr uint32_t frame_number() const { return r_host.m_frame_number; }

public:
    struct Output {
        VkImage image;
        VkSemaphore signal;
        Output(VkImage image, VkSemaphore signal)
            : image(image)
            , signal(signal)
        {
        }
    };

    virtual ~IRenderer() { }
    virtual Output draw() = 0;
    virtual void recreate_framebuffers(uint32_t frame_number) = 0;
};

class TriangleRenderer final : public twogame::vk::IRenderer {
    struct Framebuffers {
        VkImage color_buffer;
        VkImageView color_buffer_view;
        VmaAllocation color_buffer_mem;
        VkFramebuffer framebuffer;

        VkCommandPool command_pool;
        std::array<VkCommandBuffer, 2> command_buffer;
        VkSemaphore color_buffer_ready;
    };

    VkQueue m_graphics_queue;
    VkRenderPass m_render_pass;
    VkPipelineLayout m_pipeline_layout;
    VkPipeline m_pipeline;
    std::array<Framebuffers, SIMULTANEOUS_FRAMES> m_framebuffers;
    Framebuffers m_fb_discard;

    void create_graphics_pipeline();
    void create_framebuffer_sized_items();
    void create_framebuffers();

public:
    TriangleRenderer(DisplayHost* host);
    ~TriangleRenderer();

    virtual Output draw();
    virtual void recreate_framebuffers(uint32_t frame_number);
};

}
