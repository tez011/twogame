#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <atomic>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <span>
#include <vector>
#include <cglm/struct.h>
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
#define SDL_LogTrace(...) ((void)0)
#define SDL_LogDebug(...) ((void)0)
#endif
namespace twogame {

class IRenderer;
class DisplayHost;
class SceneHost;

class DisplayHost final {
    friend class SceneHost;
    static std::unique_ptr<DisplayHost> s_self;

public:
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_3;
    constexpr static int SIMULTANEOUS_FRAMES = 2;
    constexpr static VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

private:
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
    std::array<VkSemaphore, SIMULTANEOUS_FRAMES> m_sem_acquire_image;
    std::array<VkFence, SIMULTANEOUS_FRAMES> m_fence_frame;
    std::array<VkCommandBuffer, SIMULTANEOUS_FRAMES> m_present_commands;

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
    static size_t format_width(VkFormat);

    SDL_AppResult draw_frame();
};

class IRenderer {
    friend class SceneHost;

public:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;
    constexpr static int PICTUREBOOK_CAPACITY = 16; // This is as high as we can go without using Metal argument buffers, which are enabled with the update-after-bind flag.
    enum class GraphicsPipeline {
        GPass,
        MAX_VALUE,
    };
    enum class ComputePipeline {
        MAX_VALUE,
    };

private:
    VkBuffer m_uniform_buffer;
    VmaAllocation m_uniform_buffer_mem;
    std::byte* m_uniform_buffer_ptr;

    VkSampler m_sampler;

    mat4s m_perspective_projection, m_ortho_projection;
    std::vector<VkDescriptorSetLayout> m_descriptor_layouts;
    VkDescriptorPool m_graphics_descriptor_pool;
    std::array<VkDescriptorSet, SIMULTANEOUS_FRAMES> m_descriptor_set_0;
    std::array<std::array<VkDescriptorSet, static_cast<size_t>(GraphicsPipeline::MAX_VALUE)>, SIMULTANEOUS_FRAMES> m_descriptor_set_1;

protected:
    VkRenderPass m_render_pass;
    std::array<VkPipelineLayout, static_cast<size_t>(GraphicsPipeline::MAX_VALUE)> m_graphics_pipeline_layouts;
    std::array<VkPipelineLayout, static_cast<size_t>(ComputePipeline::MAX_VALUE)> m_compute_pipeline_layouts;
    std::array<VkPipeline, static_cast<size_t>(GraphicsPipeline::MAX_VALUE)> m_graphics_pipelines;
    std::array<VkPipeline, static_cast<size_t>(ComputePipeline::MAX_VALUE)> m_compute_pipelines;

    IRenderer();

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

    inline VkRenderPass render_pass() const { return m_render_pass; }
    inline VkPipelineLayout graphics_pipeline_layout(GraphicsPipeline i) const { return m_graphics_pipeline_layouts[static_cast<size_t>(i)]; }
    inline VkPipelineLayout compute_pipeline_layout(ComputePipeline i) const { return m_compute_pipeline_layouts[static_cast<size_t>(i)]; }
    inline VkPipeline graphics_pipeline(GraphicsPipeline i) const { return m_graphics_pipelines[static_cast<size_t>(i)]; }
    inline VkPipeline compute_pipeline(ComputePipeline i) const { return m_compute_pipelines[static_cast<size_t>(i)]; }
    inline mat4s projection() const { return m_perspective_projection; }
    inline mat4s ortho_projection() const { return m_ortho_projection; }
    inline VkSampler sampler() const { return m_sampler; }
    inline const VkDescriptorSetLayout& picturebook_descriptor_layout() const { return m_descriptor_layouts[2]; }
    std::span<std::byte> descriptor_buffer(int frame, int set, int binding);
    void flush_descriptor_buffers();

    void bind_pipeline(VkCommandBuffer cmd, GraphicsPipeline pass, int frame_number);
    void bind_pipeline(VkCommandBuffer cmd, ComputePipeline pass, int frame_number);
    virtual Output draw(uint32_t frame_number) = 0;
    virtual void recreate_subpass_data(uint32_t frame_number) = 0;

    void resize_frames(VkExtent2D surface_extent);
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
    struct GPass : public Subpass {
        VkImage color_buffer, depth_buffer;
        VkImageView color_buffer_view, depth_buffer_view;
        VmaAllocation color_buffer_mem, depth_buffer_mem;
    };
    using AllSubpasses = std::tuple<GPass>;
    struct FrameData {
        FrameContext ctx;
        AllSubpasses pass;
    };
    static_assert(std::tuple_size<AllSubpasses>::value == static_cast<size_t>(GraphicsPipeline::MAX_VALUE));

    VkQueue m_graphics_queue;
    std::array<FrameData, SIMULTANEOUS_FRAMES> m_frame_data;
    AllSubpasses m_pass_discard;

    void create_graphics_pipeline();
    void create_frame_data(FrameData&);
    void create_subpass_data(AllSubpasses&);
    void destroy_subpass_data(AllSubpasses&);

public:
    SimpleForwardRenderer();
    ~SimpleForwardRenderer();

    virtual Output draw(uint32_t frame_number);
    virtual void recreate_subpass_data(uint32_t frame_number);
};

}
