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
    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;

    std::array<VkBuffer, SIMULTANEOUS_FRAMES> m_instances, m_indirect_commands;
    std::array<VmaAllocation, SIMULTANEOUS_FRAMES> m_instances_mem, m_indirect_commands_mem;
    std::array<std::byte*, SIMULTANEOUS_FRAMES> m_instances_ptr, m_indirect_commands_ptr;

public:
    DuckScene();
    virtual ~DuckScene();

    virtual bool construct(twogame::IRenderer* renderer, twogame::StagingBuffer& staging, size_t pass, size_t ticket);
    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage);
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage);
    virtual void record_commands(twogame::IRenderer* renderer, uint32_t frame_number);

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass);
};

DuckScene::DuckScene()
{
    // load assets
    m_meshes.emplace_back();
    m_images.emplace_back();
}

DuckScene::~DuckScene()
{
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_instances[i], m_instances_mem[i]);
        vmaDestroyBuffer(twogame::DisplayHost::allocator(), m_indirect_commands[i], m_indirect_commands_mem[i]);
    }
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(twogame::DisplayHost::device(), *it, nullptr);
}

bool DuckScene::construct(twogame::IRenderer* renderer, twogame::StagingBuffer& staging, size_t pass, size_t ticket)
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

    m_mesh_buffer.build(m_meshes);
    std::vector<twogame::IAsset*> all_assets;
    for (auto it = m_meshes.begin(); it != m_meshes.end(); ++it)
        all_assets.push_back(&*it);
    for (auto it = m_images.begin(); it != m_images.end(); ++it)
        all_assets.push_back(&*it);

    std::vector<std::vector<twogame::IAsset*>> buckets(1);
    std::vector<size_t> bucket_usage(1);
    std::sort(all_assets.begin(), all_assets.end(), [](const twogame::IAsset* lhs, const twogame::IAsset* rhs) {
        return lhs->prepare_needs() > rhs->prepare_needs();
    });
    for (auto it = all_assets.begin(); it != all_assets.end(); ++it) {
        bool unassigned = true;
        size_t occ = (*it)->prepare_needs();
        for (size_t i = 0; i < buckets.size() && unassigned; i++) {
            if (bucket_usage[i] + occ <= twogame::SceneHost::STAGING_BUFFER_SIZE) {
                bucket_usage[i] += occ;
                buckets[i].push_back(*it);
                unassigned = false;
            }
        }
        if (unassigned) {
            bucket_usage.push_back(occ);
            buckets.emplace_back().push_back(*it);
        }
    }
    assert(buckets.size() == 1); // not dealing with multiple steps yet

    // instance and indirect buffers. the sizing is scene-specific for sure.
    VkBufferCreateInfo buffer_ci {};
    VmaAllocationCreateInfo alloc_ci {};
    VmaAllocationInfo alloc_info;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = 96 * 1;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_instances[i], &m_instances_mem[i], &alloc_info));
        m_instances_ptr[i] = static_cast<std::byte*>(alloc_info.pMappedData);
    }

    buffer_ci.size = sizeof(VkDrawIndirectCommand) * 1;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_indirect_commands[i], &m_indirect_commands_mem[i], &alloc_info));
        m_indirect_commands_ptr[i] = static_cast<std::byte*>(alloc_info.pMappedData);
    }

    // END stuff that is done prior to all construct() calls

    size_t staging_offset = 0;
    for (auto it = buckets[0].begin(); it != buckets[0].end(); ++it) {
        staging_offset += (*it)->prepare(this, staging, staging_offset);
        (*it)->post_prepare(ticket);
    }

    // BEGIN stuff done after all construct() have finished
    std::vector<VkDescriptorImageInfo> picturebook_writes(m_images.size());
    for (size_t i = 0; i < m_images.size(); i++) {
        picturebook_writes[i].sampler = VK_NULL_HANDLE;
        picturebook_writes[i].imageView = m_images[i].view();
        picturebook_writes[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    renderer->bind_picturebook(picturebook_writes);

    // this is actually scene construction, can be done whenever
    struct Instanceinfo {
        VkDeviceAddress vertex_buffer;
        VkDeviceAddress normal_buffer;
        VkDeviceAddress index_buffer;
        uint32_t material_id;
        uint32_t padding;
        mat4s model;
    };
    VkBufferDeviceAddressInfo bda_info {};
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = m_mesh_buffer.handle();
    VkDeviceAddress vertex_buffer_address = vkGetBufferDeviceAddress(twogame::DisplayHost::device(), &bda_info);
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        Instanceinfo* instances = reinterpret_cast<Instanceinfo*>(m_instances_ptr[i]);
        instances[0].vertex_buffer = vertex_buffer_address + (2 * 12636);
        instances[0].normal_buffer = vertex_buffer_address + (2 * 12636 + 32 * 2399);
        instances[0].index_buffer = vertex_buffer_address;
        instances[0].material_id = 0;
        instances[0].model = GLMS_MAT4_IDENTITY_INIT;

        VkDrawIndirectCommand* indirect_commands = reinterpret_cast<VkDrawIndirectCommand*>(m_indirect_commands_ptr[i]);
        indirect_commands[0].vertexCount = 12636; // this is actually the index count
        indirect_commands[0].instanceCount = 1;
        indirect_commands[0].firstVertex = 0;
        indirect_commands[0].firstInstance = 0;

        vmaFlushAllocation(twogame::DisplayHost::allocator(), m_instances_mem[i], 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(twogame::DisplayHost::allocator(), m_indirect_commands_mem[i], 0, VK_WHOLE_SIZE);
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

    memcpy(renderer->binding_zero_buffer(frame_number).subspan(0, sizeof(mat4)).data(), renderer->projection().raw, sizeof(mat4));
    memcpy(renderer->binding_zero_buffer(frame_number).subspan(sizeof(mat4), sizeof(mat4)).data(), view, sizeof(mat4));
    renderer->flush_binding_zero_buffer();

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
    renderer->bind_pipeline(cmd, twogame::IRenderer::GraphicsPipeline::GPass, frame_number, true);

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
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = m_instances[frame_number % SIMULTANEOUS_FRAMES];
    VkDeviceAddress instance_buffer_address = vkGetBufferDeviceAddress(twogame::DisplayHost::device(), &bda_info);
    vkCmdPushConstants(cmd, renderer->graphics_pipeline_layout(twogame::IRenderer::GraphicsPipeline::GPass), VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &instance_buffer_address);
    vkCmdDrawIndirect(cmd, m_indirect_commands[frame_number % SIMULTANEOUS_FRAMES], 0, 1, sizeof(VkDrawIndirectCommand));
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
