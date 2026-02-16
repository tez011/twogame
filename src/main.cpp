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
    // Descriptor info, from renderer/shaders. It is not clear to me that this belongs to the scene,
    // but the scene totally does define the data written to these buffers
    static constexpr size_t DESCRIPTOR_SET_COUNT = 2;
    std::array<std::array<twogame::DescriptorSet, DESCRIPTOR_SET_COUNT>, SIMULTANEOUS_FRAMES> m_descriptor_sets;
    twogame::BufferPool m_uniform_buffer;

    twogame::asset::Image m_image;
    twogame::asset::Mesh m_mesh;
    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;

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
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(twogame::DisplayHost::device(), *it, nullptr);
}

bool DuckScene::construct(twogame::IRenderer* renderer, twogame::SceneHost::StagingBuffer& staging, size_t pass, size_t ticket)
{
    size_t staging_offset = 0;
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = twogame::DisplayHost::queue_family_index();

    VkCommandBufferAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateCommandPool(twogame::DisplayHost::device(), &pool_info, nullptr, &m_draw_cmd_pool[i]));

        alloc_info.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(twogame::DisplayHost::device(), &alloc_info, m_draw_cmd[i].data()));
    }

    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        m_descriptor_sets[i][0] = renderer->pipeline(0).allocate_descriptor_set(0);
        m_descriptor_sets[i][1] = renderer->pipeline(0).allocate_descriptor_set(1);
    }
    m_uniform_buffer = twogame::BufferPool(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, sizeof(mat4), 4);
    staging_offset += m_image.prepare(staging, staging_offset);
    staging_offset += m_mesh.prepare(staging, staging_offset);

    twogame::DescriptorSet::Update update;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        update.set(m_descriptor_sets[i][0]).binding(0).write_buffer(m_uniform_buffer.buffer_handle(i));
        update.set(m_descriptor_sets[i][1]).binding(0).write_image(m_image.view(), renderer->sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    update.finish();

    m_image.post_prepare(ticket);
    m_mesh.post_prepare(ticket);
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

    mat4 view, proj_view;
    vec3 eye = { 0, 250, (float)frame_number - 500 }, toward = { 0, 100, 0 };
    glm_lookat(eye, toward, ((vec3) { 0, frame_number <= 500 ? 1.f : -1.f, 0 }), view);
    glm_mat4_mul(renderer->projection().raw, view, proj_view);

    memcpy(m_uniform_buffer.buffer_memory(frame_number % SIMULTANEOUS_FRAMES).data(), proj_view, sizeof(mat4));
    m_uniform_buffer.flush_memory({ frame_number % SIMULTANEOUS_FRAMES });

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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline(0));

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

    std::array<VkDescriptorSet, DESCRIPTOR_SET_COUNT> descriptor_sets;
    std::copy(m_descriptor_sets[frame_number % SIMULTANEOUS_FRAMES].begin(), m_descriptor_sets[frame_number % SIMULTANEOUS_FRAMES].end(), descriptor_sets.begin());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline(0).layout(), 0, descriptor_sets.size(), descriptor_sets.data(), 0, nullptr);

    std::array<VkBuffer, 4> buffers = { m_mesh.m_vertex_buffer, m_mesh.m_vertex_buffer, m_mesh.m_vertex_buffer, m_mesh.m_vertex_buffer };
    std::array<VkDeviceSize, 4> buffer_offs = { 28788, 0, 0, 57576 };
    std::array<float, 8> trs = { 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    vkCmdBindIndexBuffer(cmd, m_mesh.m_index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindVertexBuffers(cmd, 0, buffers.size(), buffers.data(), buffer_offs.data());
    vkCmdPushConstants(cmd, renderer->pipeline(0).layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, trs.size() * sizeof(decltype(trs)::value_type), trs.data());
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
