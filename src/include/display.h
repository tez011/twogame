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
class Pipeline;

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

    VkDescriptorSetLayout m_empty_descriptor_set_layout;
    VkImage m_null_image;
    VkImageView m_null_image_view;
    VmaAllocation m_null_image_mem;
    VkSampler m_null_image_sampler;

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
    static inline VkDescriptorSetLayout empty_descriptor_set_layout() { return s_self->m_empty_descriptor_set_layout; }
    static inline VkImageView null_image_view() { return s_self->m_null_image_view; }
    static inline VkSampler null_sampler() { return s_self->m_null_image_sampler; }
    static size_t format_width(VkFormat);

    SDL_AppResult draw_frame();
};

class BufferPool {
    std::vector<std::tuple<VkBuffer, VmaAllocation, VmaAllocationInfo>> m_buffers;
    std::vector<bool> m_bits;
    std::vector<bool>::iterator m_bits_it;
    size_t m_unit_size, m_count;
    VkBufferUsageFlags m_usage;
    VmaAllocationCreateFlags m_alloc_flags;

    std::vector<bool>::iterator extend();

public:
    using index_t = uint32_t;
    BufferPool() = default;
    BufferPool(VkBufferUsageFlags usage, VmaAllocationCreateFlags alloc_flags, size_t unit_size, index_t count = 0x1000);
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&);
    BufferPool& operator=(BufferPool&&) = default;
    ~BufferPool();

    index_t allocate();
    void free(index_t);

    std::tuple<VkBuffer, VkDeviceAddress, VkDeviceSize> buffer_handle(index_t);
    std::span<std::byte> buffer_memory(index_t);
    void flush_memory(std::initializer_list<index_t> indexes = {});
};

struct DescriptorBindingInfo : public VkDescriptorSetLayoutBinding {
    VkDeviceSize uniform_buffer_size;
};

class DescriptorPool;
class DescriptorSet {
    DescriptorPool* r_pool;
    VkDescriptorSet m_set;

public:
    class Update {
    public:
        class Set {
        public:
            class Binding {
                Set& r_update;
                VkWriteDescriptorSet& m_write;

            public:
                Binding(Set& update, VkWriteDescriptorSet& write, uint32_t binding, uint32_t array_element);
                Update& write_image(VkImageView, VkSampler, VkImageLayout);
                Update& write_buffer(VkBuffer, VkDeviceAddress, VkDeviceSize);
                Update& write_buffer(std::tuple<VkBuffer, VkDeviceAddress, VkDeviceSize>);
            };

        private:
            Update& r_update;
            DescriptorSet& r_set;
            VkWriteDescriptorSet& m_write;

        public:
            Set(Update& update, VkWriteDescriptorSet& write, DescriptorSet& set);
            Binding binding(uint32_t binding, uint32_t array_element = 0);
        };

    private:
        std::vector<VkWriteDescriptorSet> m_writes;
        std::list<VkDescriptorBufferInfo> m_buffers;
        std::list<VkDescriptorImageInfo> m_images;

    public:
        Update()
        {
        }
        Set set(DescriptorSet& set);
        void finish();

        friend class Binding;
    };

    DescriptorSet()
        : r_pool(nullptr)
        , m_set(VK_NULL_HANDLE)
    {
    }
    DescriptorSet(DescriptorPool& pool, VkDescriptorSet set)
        : r_pool(&pool)
        , m_set(set)
    {
    }
    ~DescriptorSet();

    inline operator VkDescriptorSet() const { return m_set; }
};

class DescriptorPool {
    friend class DescriptorSet;
    constexpr static size_t DESCRIPTOR_SETS_PER_POOL = 1024;

    VkDescriptorSetLayout r_layout;
    std::vector<DescriptorBindingInfo> m_bindings;

    VkDescriptorPool m_pool;
    std::deque<DescriptorSet> m_free_sets;
    size_t m_alloc_count;

public:
    DescriptorPool(VkDescriptorSetLayout layout, const std::vector<DescriptorBindingInfo>& bindings);
    DescriptorPool(const DescriptorPool&) = delete;
    ~DescriptorPool();

    inline const DescriptorBindingInfo& bindings(size_t i) const { return m_bindings[i]; }
    inline bool full() const { return m_alloc_count == DESCRIPTOR_SETS_PER_POOL && m_free_sets.empty(); }

    DescriptorSet allocate();
    void free(DescriptorSet&& set);
};

class Pipeline {
    friend class IRenderer;
    friend class PipelineBuilder;

    std::array<std::vector<DescriptorBindingInfo>, 4> m_descriptor_bindings;
    std::array<VkDescriptorSetLayout, 4> m_descriptor_set_layouts;
    std::array<std::list<DescriptorPool>, 4> m_descriptor_pools;

    VkPipelineLayout m_layout;
    VkPipeline m_pipeline;

    Pipeline()
        : m_layout(VK_NULL_HANDLE)
        , m_pipeline(VK_NULL_HANDLE)
    {
    }
    void create_layout(const std::array<std::vector<DescriptorBindingInfo>, 4>& descriptor_bindings,
        const std::vector<VkPushConstantRange>& push_constant_ranges);
    void create_pipeline(VkGraphicsPipelineCreateInfo&, VkPipelineCache cache = VK_NULL_HANDLE);
    void create_pipeline(VkComputePipelineCreateInfo&, VkPipelineCache cache = VK_NULL_HANDLE);

public:
    Pipeline(const Pipeline&) = delete;
    Pipeline(Pipeline&&);
    ~Pipeline();

    inline operator VkPipeline() const { return m_pipeline; }
    inline VkPipelineLayout layout() const { return m_layout; }
    inline VkDescriptorSetLayout descriptor_set_layout(size_t i) const { return m_descriptor_set_layouts[i]; }
    inline const std::vector<DescriptorBindingInfo>& binding_infos(int set) const { return m_descriptor_bindings[set]; }

    DescriptorSet allocate_descriptor_set(int set);
};

class PipelineBuilder {
    friend class IRenderer;

    bool m_is_graphics;
    VkPrimitiveTopology m_primitive_topology;
    VkBool32 m_depth_clamp;
    std::pair<VkRenderPass, uint32_t> m_render_pass;
    std::vector<VkShaderModuleCreateInfo> m_shader_modules_ci;
    std::vector<VkPipelineShaderStageCreateInfo> m_pipeline_shaders;
    std::vector<VkSpecializationInfo> m_shader_specializations;

    PipelineBuilder(const PipelineBuilder&) = delete;
    void reset(bool is_graphics);

public:
    PipelineBuilder() { }
    PipelineBuilder& new_graphics(VkRenderPass render_pass, uint32_t subpass);
    PipelineBuilder& new_compute();
    PipelineBuilder& with_shader(const uint32_t* text, size_t size, VkShaderStageFlagBits stage, const void* specialization = nullptr, size_t specialization_size = 0, std::span<const VkSpecializationMapEntry> specialization_map = {});
    PipelineBuilder& with_depth_clamp(bool);
    PipelineBuilder& with_primitive_topology(VkPrimitiveTopology);

    Pipeline build();
};

class IRenderer {
    friend class SceneHost;

    mat4s m_perspective_projection, m_ortho_projection;
    VkSampler m_sampler;

protected:
    constexpr static int SIMULTANEOUS_FRAMES = DisplayHost::SIMULTANEOUS_FRAMES;

    VkRenderPass m_render_pass;
    std::vector<Pipeline> m_pipelines;

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
    inline Pipeline& pipeline(size_t i) { return m_pipelines[i]; }
    inline mat4s projection() { return m_perspective_projection; }
    inline mat4s ortho_projection() { return m_ortho_projection; }
    inline VkSampler sampler() { return m_sampler; }

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
    SimpleForwardRenderer();
    ~SimpleForwardRenderer();

    virtual Output draw(uint32_t frame_number);
    virtual void recreate_subpass_data(uint32_t frame_number);
};

}
