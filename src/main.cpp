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
    VkDescriptorPool m_descriptor_pool;
    std::array<std::array<twogame::DescriptorSet, DESCRIPTOR_SET_COUNT>, SIMULTANEOUS_FRAMES> m_descriptor_sets;
    std::array<VkBuffer, SIMULTANEOUS_FRAMES> m_uniform_buffer;
    std::array<VmaAllocation, SIMULTANEOUS_FRAMES> m_uniform_buffer_mem;

    VkBuffer m_buffer;
    VkSampler m_sampler;
    VmaAllocation m_mem;
    twogame::asset::Image m_image;
    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;

public:
    DuckScene()
    {
    }
    virtual ~DuckScene();

    virtual bool construct(twogame::IRenderer* renderer, int pass, twogame::SceneHost::StagingBuffer& staging);
    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage);
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage);
    virtual void record_commands(twogame::IRenderer* renderer, uint32_t frame_number);

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass);
};

DuckScene::~DuckScene()
{
    vkDestroySampler(twogame::DisplayHost::instance().device(), m_sampler, nullptr);
    vmaDestroyBuffer(twogame::DisplayHost::instance().allocator(), m_buffer, m_mem);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(twogame::DisplayHost::instance().device(), *it, nullptr);

    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++)
        vmaDestroyBuffer(twogame::DisplayHost::instance().allocator(), m_uniform_buffer[i], m_uniform_buffer_mem[i]);
    vkDestroyDescriptorPool(twogame::DisplayHost::instance().device(), m_descriptor_pool, nullptr);
}

bool DuckScene::construct(twogame::IRenderer* renderer, int pass, twogame::SceneHost::StagingBuffer& staging)
{
    size_t staging_offset = 0;
    PHYSFS_File* duckdata = PHYSFS_openRead("/data/duck.bin");
    SDL_assert(duckdata);

    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = twogame::DisplayHost::instance().queue_family_index();

    VkCommandBufferAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateCommandPool(twogame::DisplayHost::instance().device(), &pool_info, nullptr, &m_draw_cmd_pool[i]));

        alloc_info.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(twogame::DisplayHost::instance().device(), &alloc_info, m_draw_cmd[i].data()));
    }

    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        m_descriptor_sets[i][0] = renderer->pipeline(0).allocate_descriptor_set(0);
        m_descriptor_sets[i][1] = renderer->pipeline(0).allocate_descriptor_set(1);
    }

    VkBufferCreateInfo buffer_info {};
    VmaAllocationCreateInfo mem_createinfo {};
    VmaAllocationInfo mem_info;
    void* mapped_buffer;
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = sizeof(mat4);
    buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++)
        VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::instance().allocator(), &buffer_info, &mem_createinfo, &m_uniform_buffer[i], &m_uniform_buffer_mem[i], nullptr));

    buffer_info.size = 102040;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_DEMAND(vmaCreateBuffer(twogame::DisplayHost::instance().allocator(), &buffer_info, &mem_createinfo, &m_buffer, &m_mem, &mem_info));

    VkMemoryPropertyFlags mem_type_flags;
    vmaGetMemoryTypeProperties(twogame::DisplayHost::instance().allocator(), mem_info.memoryType, &mem_type_flags);
    if (mem_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK_DEMAND(vmaMapMemory(twogame::DisplayHost::instance().allocator(), m_mem, &mapped_buffer));
        PHYSFS_readBytes(duckdata, mapped_buffer, buffer_info.size);
        vmaUnmapMemory(twogame::DisplayHost::instance().allocator(), m_mem);
        if ((mem_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            vmaFlushAllocation(twogame::DisplayHost::instance().allocator(), m_mem, 0, VK_WHOLE_SIZE);
    } else {
        VkBufferCopy2 copy {};
        copy.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        copy.srcOffset = copy.dstOffset = 0;
        copy.size = buffer_info.size;
        PHYSFS_readBytes(duckdata, staging.window(staging_offset).data(), buffer_info.size);
        staging.copy_buffer(m_buffer, buffer_info.size, std::span(&copy, 1), VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        staging_offset += (buffer_info.size + 15) & ~15;
    }

    m_image.prepare(staging, staging_offset);
    staging_offset += m_image.prepare_needs();

    VkSamplerCreateInfo sampler_info {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_FALSE; // TODO these are user preferences
    sampler_info.minLod = 0;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_DEMAND(vkCreateSampler(twogame::DisplayHost::instance().device(), &sampler_info, nullptr, &m_sampler));

    twogame::DescriptorSet::Update update;
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        update.set(m_descriptor_sets[i][0]).binding(0).write_buffer(m_uniform_buffer[0], 0, sizeof(mat4));
        update.set(m_descriptor_sets[i][1]).binding(0).write_image(m_image.view(), m_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    update.finish();

    PHYSFS_close(duckdata);
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
    vkResetCommandPool(twogame::DisplayHost::instance().device(), m_draw_cmd_pool[frame_number % SIMULTANEOUS_FRAMES], 0);

    // proj should come from renderer
    float cot_vertical_fov = 1.0f / SDL_tanf(35 * SDL_PI_F / 360.f);
    mat4 proj = GLM_MAT4_ZERO_INIT, view, proj_view;
    vec3 eye = { 0, 250, -400 }, toward = { 0, 100, 0 };
    glm_lookat(eye, toward, GLM_YUP, view);
    proj[0][0] = cot_vertical_fov * twogame::DisplayHost::instance().swapchain_extent().height / twogame::DisplayHost::instance().swapchain_extent().width;
    proj[1][1] = -cot_vertical_fov;
    proj[2][2] = -1.0f;
    proj[2][3] = -1.0f;
    proj[3][2] = -0.1f;
    glm_mat4_mul(proj, view, proj_view);

    void* mapped_buffer;
    vmaMapMemory(twogame::DisplayHost::instance().allocator(), m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES], &mapped_buffer);
    memcpy(mapped_buffer, proj_view, sizeof(mat4));
    vmaUnmapMemory(twogame::DisplayHost::instance().allocator(), m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES]);
    vmaFlushAllocation(twogame::DisplayHost::instance().allocator(), m_uniform_buffer_mem[frame_number % SIMULTANEOUS_FRAMES], 0, VK_WHOLE_SIZE);

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

    VkExtent2D swapchain_extent = twogame::DisplayHost::instance().swapchain_extent();
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

    std::array<VkBuffer, 4> buffers = { m_buffer, m_buffer, m_buffer, m_buffer };
    std::array<VkDeviceSize, 4> buffer_offs = { 28788, 0, 0, 57576 };
    std::array<float, 8> trs = { 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    vkCmdBindIndexBuffer(cmd, m_buffer, 76768, VK_INDEX_TYPE_UINT16);
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

// fields that could be part of appstate
struct AppState {
    twogame::SimpleForwardRenderer* renderer;
    twogame::SceneHost* stage;

    AppState()
        : renderer(nullptr)
        , stage(nullptr)
    {
    }
};

SDL_AppResult SDL_AppInit(void** _appstate, int argc, char** argv)
{
    static struct AppState appstate;
    *_appstate = &appstate;

    SDL_SetAppMetadata(APP_NAME, "0.0", "gh." SHORT_ORG_NAME "." SHORT_APP_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, ORG_NAME);
#ifdef DEBUG_BUILD
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_TRACE);
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
        appstate.renderer = new twogame::SimpleForwardRenderer;
        appstate.stage = new twogame::SceneHost(appstate.renderer, new DuckScene());
    } catch (...) {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (evt->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    appstate->stage->push_event(evt);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    appstate->stage->tick(); // TODO remove
    twogame::DisplayHost::owned().draw_frame(appstate->renderer, appstate->stage);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (appstate->stage)
        delete appstate->stage;
    if (appstate->renderer)
        delete appstate->renderer;
    twogame::DisplayHost::drop();
    if (PHYSFS_isInit())
        PHYSFS_deinit();
    SDL_Quit();
}
