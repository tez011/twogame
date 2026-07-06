#pragma once
#include <array>
#include <vector>
#include <cglm/struct.h>
#include <volk.h>
#include "core/constants.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IRenderer {
    mat4s m_perspective_projection, m_ortho_projection;
    std::vector<VkDescriptorPoolSize> m_descriptor_pool_sizes;
    std::array<VkDescriptorSetLayout, 1> m_descriptor_set_layout;
    std::array<VkSampler, 1> m_samplers;

protected:
    constexpr static auto FRAMES_IN_FLIGHT = Constants::FRAMES_IN_FLIGHT;
    VkRenderPass m_render_pass;
    std::vector<VkPipelineLayout> m_pipeline_layouts;
    std::vector<VkPipeline> m_graphics_pipelines, m_compute_pipelines;

    IRenderer();

public:
    constexpr static VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
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
    inline mat4s projection() const { return m_perspective_projection; }
    inline mat4s ortho_projection() const { return m_ortho_projection; }
    inline VkPipeline graphics_pipeline(size_t i) const { return m_graphics_pipelines.at(i); }
    inline VkPipelineLayout pipeline_layout(size_t i) const { return m_pipeline_layouts.at(i); }
    inline auto descriptor_set_layouts() const { return m_descriptor_set_layout; }
    inline auto samplers() const { return m_samplers; }

    virtual Output draw(uint32_t frame_number) = 0;
    virtual void recreate_subpass_data(uint32_t frame_number) = 0;

    VkDescriptorPool create_descriptor_pool() const;
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

    VkQueue m_graphics_queue;
    std::array<FrameData, FRAMES_IN_FLIGHT> m_frame_data;
    AllSubpasses m_pass_discard;

    void create_graphics_pipeline();
    void create_frame_data(FrameData&);
    void create_subpass_data(AllSubpasses&);
    void destroy_subpass_data(AllSubpasses&);

public:
    SimpleForwardRenderer();
    ~SimpleForwardRenderer();

    virtual Output draw(uint32_t frame_number) override;
    virtual void recreate_subpass_data(uint32_t frame_number) override;
};

}
