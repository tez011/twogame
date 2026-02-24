#define SDL_MAIN_USE_CALLBACKS
#include <iostream>
#include <cglm/cglm.h>
#include <ktx.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "display.h"
#include "physfs.h"
#include "scene.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

class DuckScene : public twogame::IScene {
    // These come from the shader
    struct ObjectData { };
    struct MaterialData {
        uint32_t base_color_texture;
    };

    std::array<VkBuffer, 2> m_object_buffer, m_model_buffer;
    std::array<VmaAllocation, 2> m_object_mem, m_model_mem;
    std::array<std::span<ObjectData>, 2> m_object_data;
    std::array<std::span<mat4s>, 2> m_model_data;
    VkBuffer m_material_buffer;
    VmaAllocation m_material_mem;
    std::span<MaterialData> m_material_data;

    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;
    VkDescriptorPool m_picturebook_pool;
    VkDescriptorSet m_picturebook;

    std::vector<std::shared_ptr<twogame::IAsset>> m_assets;
    std::vector<twogame::asset::Image*> m_images;
    std::vector<twogame::asset::Material*> m_materials;

public:
    DuckScene()
    {
    }
    virtual ~DuckScene();

    virtual bool construct(twogame::IRenderer* renderer, twogame::SceneHost::StagingBuffer& staging, size_t pass, size_t ticket);
    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage);
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage);
    virtual void record_commands(twogame::IRenderer* renderer, uint32_t frame_number);

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass);
};

DuckScene::~DuckScene()
{
    vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_object_buffer[0], m_object_mem[0]);
    vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_object_buffer[1], m_object_mem[1]);
    vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_model_buffer[0], m_model_mem[0]);
    vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_model_buffer[1], m_model_mem[1]);
    vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_material_buffer, m_material_mem);
    vkDestroyDescriptorPool(twogame::DisplayHost::device(), m_picturebook_pool, nullptr);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(twogame::DisplayHost::device(), *it, nullptr);
}

bool DuckScene::construct(twogame::IRenderer* renderer, twogame::SceneHost::StagingBuffer& staging, size_t pass, size_t ticket)
{
    VkCommandPoolCreateInfo cmd_pool_ci {};
    cmd_pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cmd_pool_ci.queueFamilyIndex = twogame::DisplayHost::queue_family_index();

    VkCommandBufferAllocateInfo cmd_buffer_ci {};
    cmd_buffer_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_buffer_ci.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    cmd_buffer_ci.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateCommandPool(twogame::DisplayHost::device(), &cmd_pool_ci, nullptr, &m_draw_cmd_pool[i]));

        cmd_buffer_ci.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(twogame::DisplayHost::device(), &cmd_buffer_ci, m_draw_cmd[i].data()));
    }

    VkDescriptorPoolCreateInfo descriptor_pool_ci {};
    VkDescriptorPoolSize descriptor_pool_size;
    descriptor_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_ci.maxSets = 1;
    descriptor_pool_ci.poolSizeCount = 1;
    descriptor_pool_ci.pPoolSizes = &descriptor_pool_size;
    descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_pool_size.descriptorCount = twogame::IRenderer::PICTUREBOOK_CAPACITY;
    VK_DEMAND(vkCreateDescriptorPool(twogame::DisplayHost::device(), &descriptor_pool_ci, nullptr, &m_picturebook_pool));

    // Load assets without constructing them yet. This is awkward. TODO improve it.
    // Walk the scene for asset references, deduplicate them, and shove them all in this data structure:
    m_assets.emplace_back(new twogame::asset::Mesh);

    std::set<twogame::IAsset*> all_assets;
    std::queue<twogame::IAsset*> asset_search_queue;
    for (auto it = m_assets.begin(); it != m_assets.end(); ++it)
        asset_search_queue.push(it->get());
    while (asset_search_queue.empty() == false) {
        if (all_assets.insert(asset_search_queue.front()).second)
            asset_search_queue.front()->push_dependents(asset_search_queue);
        asset_search_queue.pop();
    }

    std::priority_queue sorted_assets(all_assets.begin(), all_assets.end(), [](twogame::IAsset* left, twogame::IAsset* right) {
        return left->prepare_needs() < right->prepare_needs();
    });
    // TODO sort into buckets. Descending order, best-fit. PQ gives us this order.
    size_t staging_offset = 0;
    while (sorted_assets.empty() == false) {
        staging_offset += sorted_assets.top()->prepare(staging, staging_offset);
        sorted_assets.pop();
    }

    // Because all_assets is sorted, insertion in this way preserves sorted order
    for (auto it = all_assets.begin(); it != all_assets.end(); ++it) {
        switch ((*it)->type()) {
        case twogame::IAsset::Type::Image:
            m_images.push_back(static_cast<twogame::asset::Image*>(*it));
            break;
        case twogame::IAsset::Type::Material:
            m_materials.push_back(static_cast<twogame::asset::Material*>(*it));
            break;
        default:
            break;
        }
    }

    uint32_t image_count = m_images.size();
    VkDescriptorSetAllocateInfo descriptor_set_ci {};
    VkDescriptorSetVariableDescriptorCountAllocateInfo descriptor_variable_ci {};
    descriptor_set_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_set_ci.pNext = &descriptor_variable_ci;
    descriptor_set_ci.descriptorPool = m_picturebook_pool;
    descriptor_set_ci.descriptorSetCount = 1;
    descriptor_set_ci.pSetLayouts = &renderer->picturebook_descriptor_layout();
    descriptor_variable_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    descriptor_variable_ci.descriptorSetCount = 1;
    descriptor_variable_ci.pDescriptorCounts = &image_count;
    VK_DEMAND(vkAllocateDescriptorSets(twogame::DisplayHost::device(), &descriptor_set_ci, &m_picturebook));

    VkBufferCreateInfo buffer_ci {};
    VmaAllocationCreateInfo alloc_ci {};
    VmaAllocationInfo alloc_info;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = 0;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    m_object_buffer.fill(VK_NULL_HANDLE);
    m_object_mem.fill(VK_NULL_HANDLE);
    buffer_ci.size = /* total instances **/ sizeof(mat4);
    VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_model_buffer[0], &m_model_mem[0], &alloc_info));
    m_model_data[0] = std::span(static_cast<mat4s*>(alloc_info.pMappedData), 1);
    VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_model_buffer[1], &m_model_mem[1], &alloc_info));
    m_model_data[1] = std::span(static_cast<mat4s*>(alloc_info.pMappedData), 1);
    buffer_ci.size = std::max(64UL, m_materials.size() * sizeof(MaterialData));
    VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_material_buffer, &m_material_mem, &alloc_info));
    m_material_data = std::span(static_cast<MaterialData*>(alloc_info.pMappedData), m_materials.size());
    for (size_t i = 0; i < m_materials.size(); i++) {
        auto it = std::lower_bound(m_images.begin(), m_images.end(), m_materials[i]->base_color_texture());
        SDL_assert(*it == m_materials[i]->base_color_texture());
        m_material_data[i].base_color_texture = std::distance(m_images.begin(), it);
    }

    // All assets need to be prepared before creating the picture book, bound to descriptor set 2.
    VkWriteDescriptorSet picturebook_write {};
    std::vector<VkDescriptorImageInfo> picturebook_writes(m_images.size());
    for (size_t i = 0; i < m_images.size(); i++) {
        picturebook_writes[i].sampler = renderer->sampler();
        picturebook_writes[i].imageView = m_images[i]->view();
        picturebook_writes[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    picturebook_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    picturebook_write.dstSet = m_picturebook;
    picturebook_write.dstBinding = 0;
    picturebook_write.descriptorCount = picturebook_writes.size();
    picturebook_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    picturebook_write.pImageInfo = picturebook_writes.data();
    vkUpdateDescriptorSets(twogame::DisplayHost::device(), 1, &picturebook_write, 0, nullptr);

    for (auto it = all_assets.begin(); it != all_assets.end(); ++it) {
        (*it)->post_prepare(ticket);
    }
    return true;
}

void DuckScene::handle_event(const SDL_Event& evt, twogame::SceneHost* stage)
{
}

void DuckScene::tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage)
{
}

void DuckScene::record_commands(twogame::IRenderer* renderer, uint32_t frame_number)
{
    vkResetCommandPool(twogame::DisplayHost::device(), m_draw_cmd_pool[frame_number % SIMULTANEOUS_FRAMES], 0);

    mat4 view;
    vec3 eye = { 0, 250, (float)frame_number - 500 }, toward = { 0, 100, 0 };
    glm_lookat(eye, toward, ((vec3) { 0, frame_number <= 500 ? 1.f : -1.f, 0 }), view);

    memcpy(renderer->descriptor_buffer(frame_number, 0, 0).subspan(0, sizeof(mat4)).data(), renderer->projection().raw, sizeof(mat4));
    memcpy(renderer->descriptor_buffer(frame_number, 0, 0).subspan(sizeof(mat4), sizeof(mat4)).data(), view, sizeof(mat4));
    renderer->flush_descriptor_buffers();

    m_model_data[frame_number % SIMULTANEOUS_FRAMES][0] = GLMS_MAT4_IDENTITY;
    vmaFlushAllocation(twogame::DisplayHost::allocator(), m_model_mem[frame_number % SIMULTANEOUS_FRAMES], 0, VK_WHOLE_SIZE);

    VkCommandBuffer cmd = m_draw_cmd[frame_number % SIMULTANEOUS_FRAMES][0];
    VkCommandBufferBeginInfo begin_info {};
    VkCommandBufferInheritanceInfo inherit_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inherit_info;
    inherit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inherit_info.renderPass = renderer->render_pass();
    inherit_info.subpass = 0;
    VK_DEMAND(vkBeginCommandBuffer(cmd, &begin_info));
    renderer->bind_pipeline(cmd, twogame::IRenderer::GraphicsPipeline::GPass, frame_number);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->graphics_pipeline_layout(twogame::IRenderer::GraphicsPipeline::GPass), 2, 1, &m_picturebook, 0, nullptr);

    VkExtent2D swapchain_extent = twogame::DisplayHost::swapchain_extent();
    VkViewport viewport {};
    VkRect2D scissor {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = swapchain_extent.width;
    viewport.height = swapchain_extent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    scissor.offset = { 0, 0 };
    scissor.extent = swapchain_extent;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkBufferDeviceAddressInfo bda_info {};
    std::array<VkDeviceAddress, 3> pod;
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = m_model_buffer[frame_number % SIMULTANEOUS_FRAMES];
    pod[1] = vkGetBufferDeviceAddress(twogame::DisplayHost::device(), &bda_info);
    bda_info.buffer = m_material_buffer;
    pod[2] = vkGetBufferDeviceAddress(twogame::DisplayHost::device(), &bda_info);

    auto mesh = static_cast<twogame::asset::Mesh*>(m_assets[0].get());
    std::array<VkBuffer, 4> buffers = { mesh->m_vertex_buffer, mesh->m_vertex_buffer, mesh->m_vertex_buffer, mesh->m_vertex_buffer };
    std::array<VkDeviceSize, 4> buffer_offs = { 28788, 0, 0, 57576 };
    std::array<VkDeviceSize, 4> buffer_strides = { 12, 12, 0, 8 };
    vkCmdBindIndexBuffer(cmd, mesh->m_index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindVertexBuffers2(cmd, 0, buffers.size(), buffers.data(), buffer_offs.data(), nullptr, buffer_strides.data());
    vkCmdPushConstants(cmd, renderer->graphics_pipeline_layout(twogame::IRenderer::GraphicsPipeline::GPass), VK_SHADER_STAGE_ALL, 0, pod.size() * sizeof(VkDeviceAddress), pod.data());
    vkCmdDrawIndexed(cmd, 12636, 1, 0, 0, 0);
    vkEndCommandBuffer(cmd);
}

std::span<VkCommandBuffer> DuckScene::draw_commands(uint32_t frame_number, int subpass)
{
    auto& frame_commands = m_draw_cmd[frame_number % SIMULTANEOUS_FRAMES];
    switch (subpass) {
    case 0:
        return std::span(frame_commands).subspan(0, 1);
    default:
        std::abort();
    }
}

SDL_AppResult SDL_AppInit(void** _appstate, int argc, char** argv)
{
    SDL_SetAppMetadata(APP_NAME, "0.0", "gh." SHORT_ORG_NAME "." SHORT_APP_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, ORG_NAME);
#ifdef DEBUG_BUILD
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif
    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_SENSOR;
    if (volkInitialize() != VK_SUCCESS) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "volkInitialize: no loader found");
        return SDL_APP_FAILURE;
    }
    if (SDL_Init(init_flags) == false) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

#ifdef DEBUG_BUILD
    constexpr const char* rsrc_root = TWOGAME_SOURCE_ROOT "/resources";
    constexpr const char* pref_root = TWOGAME_SOURCE_ROOT "/prefs";
    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(rsrc_root, "/data", 0) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /data/: %s", rsrc_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(pref_root, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /pref/: %s", pref_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    PHYSFS_setWriteDir(pref_root);
#else
    twogame::init_filesystem(argv[0], SHORT_ORG_NAME, SHORT_APP_NAME);
    char mountpoint[4096];
    const char* base_path = SDL_GetBasePath();

    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    for (const auto& dirent : std::filesystem::directory_iterator(base_path)) {
        if (dirent.is_regular_file() == false && dirent.is_directory() == false)
            continue;

        const auto& path = dirent.path();
        if (path.has_filename() && path.has_stem() && strncasecmp(path.extension().c_str(), ".pk2", 4) == 0) {
            const char* fullpath = path.c_str();
            snprintf(mountpoint, 4096, "/%s", path.stem().c_str());
            if (PHYSFS_mount(fullpath, mountpoint, 1) == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> %s/: %s", fullpath, mountpoint, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> %s/", fullpath, mountpoint);
            }
        }
    }

    char* pref_path = SDL_GetPrefPath(SHORT_ORG_NAME, SHORT_APP_NAME);
    if (PHYSFS_mount(pref_path, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> /pref/: %s", pref_path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> /pref/", pref_path);
    }
    PHYSFS_setWriteDir(pref_path);
    SDL_free(pref_path);
#endif

    try {
        twogame::DisplayHost::init();
        twogame::SceneHost::init(new twogame::SimpleForwardRenderer, new DuckScene);
    } catch (...) {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    if (evt->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    twogame::SceneHost::push_event(evt);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    twogame::DisplayHost::owned().draw_frame();
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    twogame::SceneHost::drop();
    twogame::DisplayHost::drop();
    if (PHYSFS_isInit())
        PHYSFS_deinit();
    SDL_Quit();
}
