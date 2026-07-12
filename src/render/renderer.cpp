#include "renderer.h"
#include <bit>
#include "core/debug.h"
#include "display_host.h"
#include "embedded_shaders.h"
#include "scene/scene_host.h"

namespace twogame {

IRenderer::IRenderer()
    : m_perspective_projection(GLMS_MAT4_ZERO_INIT)
    , m_ortho_projection(GLMS_MAT4_ZERO_INIT)
    , m_render_pass(VK_NULL_HANDLE)
{
    VkPhysicalDeviceProperties hwd_props;
    vkGetPhysicalDeviceProperties(DisplayHost::hardware_device(), &hwd_props);
    m_max_msaa = std::bit_floor(hwd_props.limits.framebufferColorSampleCounts & hwd_props.limits.framebufferDepthSampleCounts);
    m_max_msaa = std::min(m_max_msaa, 8U);

    VkPhysicalDeviceFeatures hwd_features;
    vkGetPhysicalDeviceFeatures(DisplayHost::hardware_device(), &hwd_features);
    if (hwd_features.sampleRateShading)
        m_sample_rate_shading = 0.3f;
    else
        m_sample_rate_shading = 0;

    VkSamplerCreateInfo sampler_info {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_TRUE;
    sampler_info.maxAnisotropy = std::min(16.f, hwd_props.limits.maxSamplerAnisotropy);
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_DEMAND(vkCreateSampler(DisplayHost::device(), &sampler_info, nullptr, &m_samplers[0]));

    VkDescriptorSetLayoutCreateInfo binding_layout_ci {};
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_ci {};
    std::array<VkDescriptorSetLayoutBinding, 5> bindings {};
    std::array<VkDescriptorBindingFlags, std::size(bindings)> binding_flags {};
    binding_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    binding_layout_ci.pNext = &binding_flags_ci;
    binding_layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    binding_flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    binding_layout_ci.bindingCount = binding_flags_ci.bindingCount = bindings.size();
    binding_layout_ci.pBindings = bindings.data();
    binding_flags_ci.pBindingFlags = binding_flags.data();
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[4].descriptorCount = Constants::PICTUREBOOK_CAPACITY;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding_flags[4] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VK_DEMAND(vkCreateDescriptorSetLayout(DisplayHost::device(), &binding_layout_ci, nullptr, &m_descriptor_set_layout[0]));

    VkPipelineLayoutCreateInfo pipeline_layout_ci {};
    VkPushConstantRange push_constant_range {};
    pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_ci.setLayoutCount = m_descriptor_set_layout.size();
    pipeline_layout_ci.pSetLayouts = m_descriptor_set_layout.data();
    pipeline_layout_ci.pushConstantRangeCount = 1;
    pipeline_layout_ci.pPushConstantRanges = &push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_ALL;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(VkDeviceAddress);
    VK_DEMAND(vkCreatePipelineLayout(DisplayHost::device(), &pipeline_layout_ci, nullptr, &m_pipeline_layouts.emplace_back()));

    m_descriptor_pool_sizes.resize(bindings.size());
    for (size_t i = 0; i < bindings.size(); i++) {
        m_descriptor_pool_sizes[i].type = bindings[i].descriptorType;
        m_descriptor_pool_sizes[i].descriptorCount = bindings[i].descriptorCount * FRAMES_IN_FLIGHT;
    }

    resize_frames(DisplayHost::swapchain_extent());
}

IRenderer::~IRenderer()
{
    for (auto it = m_graphics_pipelines.begin(); it != m_graphics_pipelines.end(); ++it)
        vkDestroyPipeline(DisplayHost::device(), *it, nullptr);
    for (auto it = m_compute_pipelines.begin(); it != m_compute_pipelines.end(); ++it)
        vkDestroyPipeline(DisplayHost::device(), *it, nullptr);
    for (auto it = m_pipeline_layouts.begin(); it != m_pipeline_layouts.end(); ++it)
        vkDestroyPipelineLayout(DisplayHost::device(), *it, nullptr);
    for (auto it = m_descriptor_set_layout.begin(); it != m_descriptor_set_layout.end(); ++it)
        vkDestroyDescriptorSetLayout(DisplayHost::device(), *it, nullptr);
    vkDestroyRenderPass(DisplayHost::device(), m_render_pass, nullptr);
    for (auto it = m_samplers.begin(); it != m_samplers.end(); ++it)
        vkDestroySampler(DisplayHost::device(), *it, nullptr);
}

VkDescriptorPool IRenderer::create_descriptor_pool() const
{
    VkDescriptorPool pool;
    VkDescriptorPoolCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createinfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    createinfo.maxSets = FRAMES_IN_FLIGHT;
    createinfo.poolSizeCount = m_descriptor_pool_sizes.size();
    createinfo.pPoolSizes = m_descriptor_pool_sizes.data();
    VK_DEMAND(vkCreateDescriptorPool(DisplayHost::device(), &createinfo, nullptr, &pool));
    return pool;
}

void IRenderer::resize_frames(VkExtent2D surface_extent)
{
    constexpr float vertical_fov = Constants::VERTICAL_FOV * M_PI / 180.0f;
    const float cot_vertical_fov = 1.f / SDL_tanf(0.5f * vertical_fov);
    m_perspective_projection.raw[0][0] = cot_vertical_fov * surface_extent.height / surface_extent.width;
    m_perspective_projection.raw[1][1] = -cot_vertical_fov;
    m_perspective_projection.raw[2][2] = 0.0f; // infinite far plane
    m_perspective_projection.raw[2][3] = -1.0f; // Right-handed look at -Z
    m_perspective_projection.raw[3][2] = 0.1f; // near Z plane

    m_ortho_projection.raw[0][0] = 2.f / surface_extent.width;
    m_ortho_projection.raw[1][1] = -2.f / surface_extent.height;
    m_ortho_projection.raw[3][0] = -1.f;
    m_ortho_projection.raw[3][1] = 1.f;
    m_ortho_projection.raw[3][2] = 1.f;
    m_ortho_projection.raw[3][3] = 1.f;
}

SimpleForwardRenderer::SimpleForwardRenderer()
{
    vkGetDeviceQueue(DisplayHost::device(), DisplayHost::queue_family_index(), 0, &m_graphics_queue);

    std::apply([](auto&... subpasses) {
        (memset(&subpasses, 0, sizeof(subpasses)), ...);
    },
        m_pass_discard);
    create_graphics_pipeline();
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        memset(&it->ctx, 0, sizeof(FrameContext));
        create_frame_data(*it);
    }
}

SimpleForwardRenderer::~SimpleForwardRenderer()
{
    vkDeviceWaitIdle(DisplayHost::device());

    destroy_subpass_data(m_pass_discard);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        destroy_subpass_data(it->pass);
        vkDestroySemaphore(DisplayHost::device(), it->ctx.ready, nullptr);
        vkDestroyCommandPool(DisplayHost::device(), it->ctx.command_pool, nullptr);
    }
}

void SimpleForwardRenderer::create_graphics_pipeline()
{
    std::array<VkAttachmentDescription2, 4> attachments {};
    attachments[0].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[0].format = DisplayHost::swapchain_format();
    attachments[0].samples = static_cast<VkSampleCountFlagBits>(m_max_msaa);
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = static_cast<VkSampleCountFlagBits>(m_max_msaa);
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[2].format = DisplayHost::swapchain_format();
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[3].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachments[3].format = DEPTH_FORMAT;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference2 msaa_color_att { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT },
        msaa_depth_att { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT },
        resolved_color_att { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT },
        resolved_depth_att { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT };

    VkSubpassDescriptionDepthStencilResolve depth_resolve_info {};
    depth_resolve_info.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    depth_resolve_info.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth_resolve_info.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    depth_resolve_info.pDepthStencilResolveAttachment = &resolved_depth_att;

    std::array<VkSubpassDescription2, 1> subpasses {};
    subpasses[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = &msaa_color_att;
    subpasses[0].pDepthStencilAttachment = &msaa_depth_att;
    subpasses[0].pResolveAttachments = &resolved_color_att;
    subpasses[0].pNext = &depth_resolve_info;

    std::array<VkSubpassDependency2, 1> deps {};
    deps[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    deps[0].srcSubpass = 0;
    deps[0].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;

    VkRenderPassCreateInfo2 render_pass_ci {};
    render_pass_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    render_pass_ci.attachmentCount = attachments.size();
    render_pass_ci.pAttachments = attachments.data();
    render_pass_ci.subpassCount = subpasses.size();
    render_pass_ci.pSubpasses = subpasses.data();
    render_pass_ci.dependencyCount = deps.size();
    render_pass_ci.pDependencies = deps.data();
    VK_DEMAND(vkCreateRenderPass2(DisplayHost::device(), &render_pass_ci, nullptr, &m_render_pass));

    std::array<VkShaderModule, 2> shader_modules;
    VkShaderModuleCreateInfo shader_module_info {};
    shader_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_module_info.codeSize = shaders::basic_vert_size;
    shader_module_info.pCode = shaders::basic_vert_spv;
    VK_DEMAND(vkCreateShaderModule(DisplayHost::device(), &shader_module_info, nullptr, &shader_modules[0]));
    shader_module_info.codeSize = shaders::basic_frag_size;
    shader_module_info.pCode = shaders::basic_frag_spv;
    VK_DEMAND(vkCreateShaderModule(DisplayHost::device(), &shader_module_info, nullptr, &shader_modules[1]));

    std::array<VkPipelineShaderStageCreateInfo, 2> pipeline_shaders {};
    pipeline_shaders[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    pipeline_shaders[0].module = shader_modules[0];
    pipeline_shaders[0].pName = "main";
    pipeline_shaders[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    pipeline_shaders[1].module = shader_modules[1];
    pipeline_shaders[1].pName = "main";

    std::array<VkPipelineVertexInputStateCreateInfo, 1> vertex_input_info {};
    vertex_input_info[0].sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assy_info {};
    input_assy_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assy_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_info {};
    viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_info.viewportCount = 1;
    viewport_info.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer_info {};
    rasterizer_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer_info.depthClampEnable = VK_FALSE;
    rasterizer_info.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer_info.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer_info.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample_info {};
    multisample_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_info.rasterizationSamples = static_cast<VkSampleCountFlagBits>(m_max_msaa);
    multisample_info.sampleShadingEnable = m_sample_rate_shading > 0.f;
    multisample_info.minSampleShading = m_sample_rate_shading;

    VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
    depth_stencil_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_info.depthTestEnable = VK_TRUE;
    depth_stencil_info.depthWriteEnable = VK_TRUE;
    depth_stencil_info.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depth_stencil_info.depthBoundsTestEnable = VK_FALSE;
    depth_stencil_info.stencilTestEnable = VK_FALSE;

    std::array<VkPipelineColorBlendAttachmentState, 1> color_blend_atts {};
    VkPipelineColorBlendStateCreateInfo color_blend_info {};
    color_blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_info.attachmentCount = color_blend_atts.size();
    color_blend_info.pAttachments = color_blend_atts.data();
    color_blend_atts[0].blendEnable = VK_FALSE;
    color_blend_atts[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineDynamicStateCreateInfo dynamic_state_info {};
    auto dynamic_state_set = std::to_array<VkDynamicState>({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR });
    dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_info.dynamicStateCount = dynamic_state_set.size();
    dynamic_state_info.pDynamicStates = dynamic_state_set.data();

    std::array<VkGraphicsPipelineCreateInfo, 1> graphics_pipeline_ci {};
    graphics_pipeline_ci[0].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    graphics_pipeline_ci[0].stageCount = 2;
    graphics_pipeline_ci[0].pStages = &pipeline_shaders[0];
    graphics_pipeline_ci[0].pVertexInputState = &vertex_input_info[0];
    graphics_pipeline_ci[0].pInputAssemblyState = &input_assy_info;
    graphics_pipeline_ci[0].pViewportState = &viewport_info;
    graphics_pipeline_ci[0].pRasterizationState = &rasterizer_info;
    graphics_pipeline_ci[0].pMultisampleState = &multisample_info;
    graphics_pipeline_ci[0].pDepthStencilState = &depth_stencil_info;
    graphics_pipeline_ci[0].pColorBlendState = &color_blend_info;
    graphics_pipeline_ci[0].pDynamicState = &dynamic_state_info;
    graphics_pipeline_ci[0].layout = m_pipeline_layouts[0];
    graphics_pipeline_ci[0].renderPass = m_render_pass;
    graphics_pipeline_ci[0].subpass = 0;
    m_graphics_pipelines.resize(graphics_pipeline_ci.size());
    VK_DEMAND(vkCreateGraphicsPipelines(DisplayHost::device(), DisplayHost::pipeline_cache(), graphics_pipeline_ci.size(), graphics_pipeline_ci.data(), nullptr, m_graphics_pipelines.data()));

    std::array<VkComputePipelineCreateInfo, 0> compute_pipeline_ci {};
    if constexpr (compute_pipeline_ci.empty() == false)
        VK_DEMAND(vkCreateComputePipelines(DisplayHost::device(), DisplayHost::pipeline_cache(), compute_pipeline_ci.size(), compute_pipeline_ci.data(), nullptr, m_compute_pipelines.data()));

    for (auto it = shader_modules.begin(); it != shader_modules.end(); ++it)
        vkDestroyShaderModule(DisplayHost::device(), *it, nullptr);
}

void SimpleForwardRenderer::create_frame_data(FrameData& frame)
{
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = DisplayHost::queue_family_index();
    VK_DEMAND(vkCreateCommandPool(DisplayHost::device(), &pool_info, nullptr, &frame.ctx.command_pool));

    VkCommandBufferAllocateInfo allocinfo {};
    allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocinfo.commandPool = frame.ctx.command_pool;
    allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocinfo.commandBufferCount = 1;
    VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::device(), &allocinfo, &frame.ctx.command_container));

    VkSemaphoreCreateInfo sem_info {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_DEMAND(vkCreateSemaphore(DisplayHost::device(), &sem_info, nullptr, &frame.ctx.ready));

    create_subpass_data(frame.pass);
}

void SimpleForwardRenderer::create_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<GPass>(subpasses);
        VmaAllocationCreateInfo mem_createinfo {};
        VkImageCreateInfo i_createinfo {};
        VkImageViewCreateInfo iv_createinfo {};

        i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        i_createinfo.imageType = VK_IMAGE_TYPE_2D;
        i_createinfo.format = DisplayHost::swapchain_format();
        i_createinfo.extent.width = DisplayHost::swapchain_extent().width;
        i_createinfo.extent.height = DisplayHost::swapchain_extent().height;
        i_createinfo.extent.depth = 1;
        i_createinfo.mipLevels = 1;
        i_createinfo.arrayLayers = 1;
        i_createinfo.samples = static_cast<VkSampleCountFlagBits>(m_max_msaa);
        i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        mem_createinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.msaa_color.image, &pass.msaa_color.mem, nullptr));
        iv_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_createinfo.subresourceRange.baseMipLevel = 0;
        iv_createinfo.subresourceRange.levelCount = 1;
        iv_createinfo.subresourceRange.baseArrayLayer = 0;
        iv_createinfo.subresourceRange.layerCount = 1;
        iv_createinfo.image = pass.msaa_color.image;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.msaa_color.view));

        i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
        i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.resolve_color.image, &pass.resolve_color.mem, nullptr));
        iv_createinfo.image = pass.resolve_color.image;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.resolve_color.view));

        i_createinfo.format = DEPTH_FORMAT;
        i_createinfo.samples = static_cast<VkSampleCountFlagBits>(m_max_msaa);
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.msaa_depth.image, &pass.msaa_depth.mem, nullptr));
        iv_createinfo.format = i_createinfo.format;
        iv_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        iv_createinfo.image = pass.msaa_depth.image;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.msaa_depth.view));

        i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
        i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK_DEMAND(vmaCreateImage(DisplayHost::allocator(), &i_createinfo, &mem_createinfo, &pass.resolve_depth.image, &pass.resolve_depth.mem, nullptr));
        iv_createinfo.image = pass.resolve_depth.image;
        VK_DEMAND(vkCreateImageView(DisplayHost::device(), &iv_createinfo, nullptr, &pass.resolve_depth.view));

        std::array<VkImageView, 4> fb_attachments = { pass.msaa_color.view, pass.msaa_depth.view, pass.resolve_color.view, pass.resolve_depth.view };
        VkFramebufferCreateInfo fb_createinfo {};
        fb_createinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_createinfo.renderPass = m_render_pass;
        fb_createinfo.attachmentCount = fb_attachments.size();
        fb_createinfo.pAttachments = fb_attachments.data();
        fb_createinfo.width = DisplayHost::swapchain_extent().width;
        fb_createinfo.height = DisplayHost::swapchain_extent().height;
        fb_createinfo.layers = 1;
        VK_DEMAND(vkCreateFramebuffer(DisplayHost::device(), &fb_createinfo, nullptr, &pass.framebuffer));
    }
}

void SimpleForwardRenderer::destroy_subpass_data(AllSubpasses& subpasses)
{
    {
        auto& pass = std::get<GPass>(subpasses);
        vkDestroyFramebuffer(DisplayHost::device(), pass.framebuffer, nullptr);
        vkDestroyImageView(DisplayHost::device(), pass.msaa_color.view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.msaa_color.image, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.msaa_color.mem);
        vkDestroyImageView(DisplayHost::device(), pass.msaa_depth.view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.msaa_depth.image, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.msaa_depth.mem);
        vkDestroyImageView(DisplayHost::device(), pass.resolve_color.view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.resolve_color.image, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.resolve_color.mem);
        vkDestroyImageView(DisplayHost::device(), pass.resolve_depth.view, nullptr);
        vkDestroyImage(DisplayHost::device(), pass.resolve_depth.image, nullptr);
        vmaFreeMemory(DisplayHost::allocator(), pass.resolve_depth.mem);
    }
}

IRenderer::Output SimpleForwardRenderer::draw(uint32_t frame_number)
{
    const FrameData& frame = m_frame_data[frame_number % m_frame_data.size()];
    vkResetCommandPool(DisplayHost::device(), frame.ctx.command_pool, 0);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(frame.ctx.command_container, &begin_info));

    VkRenderPassBeginInfo render_pass_begin {};
    std::array<VkClearValue, 2> clear_values = { { { { { 0.9375f, 0.6953f, 0.7344f, 1.0f } } }, { { { 0.0f, 0.0f } } } } };
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = m_render_pass;
    render_pass_begin.framebuffer = std::get<0>(frame.pass).framebuffer;
    render_pass_begin.renderArea.offset = { 0, 0 };
    render_pass_begin.renderArea.extent = DisplayHost::swapchain_extent();
    render_pass_begin.clearValueCount = clear_values.size();
    render_pass_begin.pClearValues = clear_values.data();
    SceneHost::wait_frame(frame_number);

    for (size_t i = 0; i < std::tuple_size<AllSubpasses>::value; i++) {
        if (i == 0)
            vkCmdBeginRenderPass(frame.ctx.command_container, &render_pass_begin, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        else
            vkCmdNextSubpass(frame.ctx.command_container, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        SceneHost::execute_draws(frame.ctx.command_container, frame_number, i);
    }
    vkCmdEndRenderPass(frame.ctx.command_container);
    VK_DEMAND(vkEndCommandBuffer(frame.ctx.command_container));

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 0;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.ctx.command_container;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.ctx.ready;
    VK_DEMAND(vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE));
    return IRenderer::Output(std::get<0>(frame.pass).resolve_color.image, frame.ctx.ready);
}

void SimpleForwardRenderer::recreate_subpass_data(uint32_t frame_number)
{
    destroy_subpass_data(m_pass_discard);
    destroy_subpass_data(m_frame_data[frame_number % 2].pass);

    std::swap(m_pass_discard, m_frame_data[(frame_number + 1) % 2].pass);
    for (auto it = m_frame_data.begin(); it != m_frame_data.end(); ++it) {
        create_subpass_data(it->pass);
    }
}

}
