#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>
#include <cglm/mat4.h>
#include <entt/core/type_traits.hpp>
#include <SDL.h>
#include <spdlog/spdlog.h>
#include "util.h"
#include "vk_mem_alloc.h"

#ifdef TWOGAME_DEBUG_BUILD
#define VK_CHECK(X) VK_CHECK_3(X, __FILE__, __LINE__)
#define VK_CHECK_3(X, F, L)                                                                   \
    do {                                                                                      \
        VkResult __bc_res;                                                                    \
        if ((__bc_res = (X)) != VK_SUCCESS) {                                                 \
            spdlog::critical("assert at {}:{}: {}: {}", F, L, #X, fmt::underlying(__bc_res)); \
            std::terminate();                                                                 \
        }                                                                                     \
    } while (0)
#else
#define VK_CHECK(X)              \
    do {                         \
        if ((X) != VK_SUCCESS) { \
            std::terminate();    \
        }                        \
    } while (0)
#endif

class Twogame;

namespace twogame::asset {
class Shader;
}

namespace twogame::vk_destructible {

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
struct Type {
};
#define X(T)                                               \
    template <>                                            \
    struct Type<T> {                                       \
        static constexpr Types t() { return Types::D##T; } \
    };
DESTRUCTIBLE_TYPES
#undef X

}

namespace twogame::descriptor_storage {

typedef struct {
    mat4 proj;
    mat4 view;
} uniform_s1i0_t;

}

namespace twogame {

class Scene;

class Renderer final {
    friend class asset::Shader;

public:
    constexpr static uint32_t API_VERSION = VK_API_VERSION_1_2;

    enum class DescriptorSetSlot {
        // Descriptor set 0: per-frame
        // Descriptor set 1: per-pass
        ProjectionView = 0,
        // Descriptor set 2: per-object
        ModelMatrix = 0,
        BoneMatrices,
        ShapeKeyWeights,
        PositionDisplacements,
        NormalDisplacements,
    };

private:
    using Descriptor2SlotBindingTypes = entt::type_list<uint64_t, uint64_t, uint64_t, VkImageView, VkImageView>;
    static PFN_vkDestroyDebugUtilsMessengerEXT s_vkDestroyDebugUtilsMessenger;

    enum class QueueFamily {
        Universal,
        Compute,
        Transfer,
        MAX_VALUE,
    };
    enum class CommandBuffer {
        RenderOneStage,
        BlitToSwapchain,
        MAX_VALUE,
    };
    enum class RenderAttachment {
        ColorBuffer,
        DepthBuffer,
        MAX_VALUE,
    };

    Twogame* m_twogame;
    SDL_Window* m_window;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_hwd = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator;
    VkPhysicalDeviceLimits m_device_limits {};
    VkSwapchainKHR m_swapchain;
    VkSurfaceFormatKHR m_swapchain_format;
    VkExtent2D m_swapchain_extent;
    VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout;
    std::vector<VkImage> m_swapchain_images;
    std::array<uint32_t, static_cast<size_t>(QueueFamily::MAX_VALUE)> m_queue_families;
    std::array<VkQueue, static_cast<size_t>(QueueFamily::MAX_VALUE)> m_queues;
    std::array<std::array<std::vector<VkCommandPool>, static_cast<size_t>(QueueFamily::MAX_VALUE)>, 2> m_command_pools;
    std::array<std::array<VkCommandBuffer, static_cast<size_t>(CommandBuffer::MAX_VALUE)>, 2> m_command_buffers;
    VkCommandPool m_command_pool_transient;
    VkCommandBuffer m_cbuf_asset_prepare;
    VkSampler m_active_sampler;
    VkSampler m_morph_sampler;
    VkImage m_dummy_image;
    VmaAllocation m_dummy_image_allocation;
    VkImageView m_dummy_image_view_2d;

    uint64_t m_frame_number = 0;
    bool m_mip_filter = true;
    int m_multisample_count = 1;
    float m_sample_shading = 0;
    float m_requested_anisotropy = 0;
    float m_cot_vertical_fov = 1.0f / tanf(glm_rad(45.f) * 0.5f);

    std::array<VkRenderPass, 1> m_render_pass;
    std::array<std::array<VkFramebuffer, 1>, 2> m_framebuffers;
    std::array<std::array<VkImage, static_cast<size_t>(RenderAttachment::MAX_VALUE)>, 2> m_render_atts;
    std::array<std::array<VkImageView, static_cast<size_t>(RenderAttachment::MAX_VALUE)>, 2> m_render_att_views;
    std::array<std::array<VmaAllocation, static_cast<size_t>(RenderAttachment::MAX_VALUE)>, 2> m_render_att_allocs;
    std::array<std::queue<std::pair<uint64_t, vk_destructible::Types>>, 4> m_trash;

    constexpr static size_t DS1_INSTANCES = 1, DS2_BUFFERS = 3;
    VkDescriptorSetLayout m_ds0_layout, m_ds1_layout;
    std::array<VkPushConstantRange, 1> m_push_constant_layout;
    std::array<VkDescriptorPool, 2> m_ds01_pool;
    std::array<VkDescriptorSet, 2> m_ds0;
    std::array<std::array<VkDescriptorSet, DS1_INSTANCES>, 2> m_ds1;
    std::array<vk::buffer, DS1_INSTANCES> m_ds1_buffers;
    std::array<vk::BufferPool*, DS2_BUFFERS> m_ds2_buffer_pool;
    vk::DescriptorPool* m_ds2_pool;

    std::array<VkSemaphore, 2> m_sem_image_available, m_sem_render_finished, m_sem_blit_finished;
    std::array<VkFence, 2> m_fence_frame;
    VkFence m_fence_assets_prepared;

    mat4 m_projection;

    void create_instance();
    void create_debug_messenger();
    void pick_physical_device();
    void create_logical_device();
    void create_allocator();
    void create_swapchain(VkSwapchainKHR old_swapchain);
    void create_render_pass();
    void create_framebuffers();
    void create_pipeline_cache();
    void create_descriptor_sets();
    void create_command_buffers();
    void create_synchronizers();
    void create_sampler();

    void release_freed_items(int bucket);
    void recreate_swapchain();
    void write_pipeline_cache();
    uint64_t dummy_descriptor(DescriptorSetSlot) const;

    template <typename T, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void defer_free(T i)
    {
        m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
    }
    template <typename T, size_t N, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void defer_free(std::array<T, N>& c)
    {
        for (const T& i : c)
            m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
        c.fill(VK_NULL_HANDLE);
    }
    template <typename T, vk_destructible::Types I = vk_destructible::Type<T>::t()>
    void defer_free(std::vector<T>& c)
    {
        for (const T& i : c)
            m_trash[m_frame_number % 4].emplace(uint64_t(i), I);
        c.clear();
    }

public:
    Renderer(Twogame*, SDL_Window*);
    ~Renderer();

    int32_t acquire_image();
    void draw(Scene* scene);
    void next_frame(uint32_t image_index);
    void wait_idle();
    inline uint64_t current_frame() const { return m_frame_number; }

    const VkPhysicalDevice& hwd() const { return m_hwd; }
    const VkDevice& device() const { return m_device; }
    const VmaAllocator& allocator() const { return m_allocator; }
    const VkSampler& active_sampler() const { return m_active_sampler; }
    const VkSampler& morph_sampler() const { return m_morph_sampler; }
    const VkPhysicalDeviceLimits& limits() const { return m_device_limits; }
    const VkPipelineLayout& pipeline_layout() const { return m_pipeline_layout; }
    VkPipelineLayout create_pipeline_layout(VkDescriptorSetLayout material_layout) const;

    using perobject_descriptor_buffers_t = std::array<vk::BufferPool::index_t, DS2_BUFFERS * 2>;
    bool null_descriptor_enabled() const { return m_dummy_image != VK_NULL_HANDLE; }
    const vk::BufferPool* perobject_buffer_pool(DescriptorSetSlot i) const { return m_ds2_buffer_pool[static_cast<size_t>(i)]; }
    void create_perobject_descriptors(std::array<VkDescriptorSet, 2>& sets, perobject_descriptor_buffers_t& buffers);
    void free_perobject_descriptors(std::array<VkDescriptorSet, 2>& sets, perobject_descriptor_buffers_t& buffers);
    template <DescriptorSetSlot SLOT, typename T = entt::type_list_element_t<static_cast<size_t>(SLOT), Descriptor2SlotBindingTypes>>
    T dummy_descriptor() const
    {
        return reinterpret_cast<T>(dummy_descriptor(SLOT));
    }
};

}
