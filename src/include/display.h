#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <atomic>
#include <vector>
#include <SDL3/SDL.h>
#include <volk.h>
#include "vk_mem_alloc.h"

#ifdef DEBUG_BUILD
#include <vulkan/vk_enum_string_helper.h>
#define VK_DEMAND(X) VK_DEMAND_4P(X, SDL_FUNCTION, SDL_FILE, SDL_LINE)
#define VK_DEMAND_4P(X, N, F, L)                                                                                       \
    do {                                                                                                               \
        VkResult __r;                                                                                                  \
        if ((__r = (X)) != VK_SUCCESS) {                                                                               \
            SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "assert in %s at %s:%d: %s: %s", N, F, L, #X, string_VkResult(__r)); \
            SDL_TriggerBreakpoint();                                                                                   \
        }                                                                                                              \
    } while (0)
#else
#define VK_DEMAND(X)                 \
    do {                             \
        if ((X) != VK_SUCCESS) {     \
            SDL_TriggerBreakpoint(); \
        }                            \
    } while (0)
#endif
namespace twogame {

class IRenderer;
class IScene;
class DisplayHost;
class SceneHost;

enum class QueueType {
    Graphics,
    Compute,
    Transfer,
    MAX_VALUE,
};

class DisplayHost final {
    friend class IRenderer;
    friend class SceneHost;

public:
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_3;
    constexpr static int SIMULTANEOUS_FRAMES = 2;

private:
    std::atomic_uint32_t m_frame_number = 0;
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
    DisplayHost();
    ~DisplayHost();

    inline constexpr VkDevice device() const { return m_device; }
    inline constexpr VmaAllocator allocator() const { return m_allocator; }
    inline constexpr uint32_t queue_family_index(QueueType t) { return m_queue_family_indexes[static_cast<size_t>(t)]; }

    SDL_AppResult draw_frame(IRenderer*, SceneHost*);
};

class IRenderer {
    friend class SceneHost;

protected:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;

    DisplayHost& r_host;
    VkRenderPass m_render_pass;
    std::vector<VkDescriptorSetLayout> m_descriptor_layout;
    std::vector<VkPipelineLayout> m_pipeline_layout;
    std::vector<VkPipeline> m_pipeline;

    IRenderer(DisplayHost* host)
        : r_host(*host)
        , m_render_pass(VK_NULL_HANDLE)
    {
    }

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

    virtual ~IRenderer();

    inline constexpr VkPhysicalDevice hardware_device() const { return r_host.m_hwd; }
    inline constexpr VkDevice device() const { return r_host.m_device; }
    inline constexpr VmaAllocator allocator() const { return r_host.m_allocator; }
    inline constexpr VkFormat swapchain_format() const { return r_host.m_swapchain_format; }
    inline constexpr VkExtent2D swapchain_extent() const { return r_host.m_swapchain_extent; }
    inline constexpr VkFormat depth_format() const { return r_host.m_depth_format; }
    inline constexpr uint32_t queue_family_index(QueueType t) { return r_host.m_queue_family_indexes[static_cast<size_t>(t)]; }
    inline VkRenderPass render_pass() const { return m_render_pass; }
    inline VkPipeline pipeline(size_t i) const { return m_pipeline[i]; }
    inline VkPipelineLayout pipeline_layout(size_t i) const { return m_pipeline_layout[i]; }
    inline VkDescriptorSetLayout descriptor_set_layout(size_t i) const { return m_descriptor_layout[i]; }

    virtual Output draw(SceneHost*, uint32_t frame_number) = 0;
    virtual void recreate_subpass_data(uint32_t frame_number) = 0;
};

class SimpleForwardRenderer final : public IRenderer {
    struct FrameContext {
        VkCommandPool command_pool;
        VkCommandBuffer command_container;
        VkSemaphore ready;
    };
    struct Subpass {
        VkFramebuffer framebuffer;
    };
    struct Subpass0 : public Subpass {
        VkImage color_buffer, depth_buffer;
        VkImageView color_buffer_view, depth_buffer_view;
        VmaAllocation color_buffer_mem, depth_buffer_mem;
    };
    using AllSubpasses = std::tuple<Subpass0>;
    struct FrameData {
        FrameContext ctx;
        AllSubpasses pass;
    };

    VkQueue m_graphics_queue;
    std::array<FrameData, SIMULTANEOUS_FRAMES> m_frame_data;
    AllSubpasses m_pass_discard;

    void create_graphics_pipeline();
    void create_frame_data(FrameData&);
    void create_subpass_data(AllSubpasses&);
    void destroy_subpass_data(AllSubpasses&);

public:
    SimpleForwardRenderer(DisplayHost* host);
    ~SimpleForwardRenderer();

    virtual Output draw(SceneHost*, uint32_t frame_number);
    virtual void recreate_subpass_data(uint32_t frame_number);
};

}
