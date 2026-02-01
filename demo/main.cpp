#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "twogame.h"
#include "twogame_vk.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

class TriangleScene : public twogame::vk::IScene {
    static constexpr float vb_data[] = {
        // position
        0.0,
        -0.5,
        0.0,
        0.5,
        0.5,
        0.0,
        -0.5,
        0.5,
        0.0,

        0.0,

        // color
        0.0,
        1.0,
        1.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };

protected:
    VkBuffer m_buffer, m_staging_buffer;
    VmaAllocation m_mem, m_staging_mem;

    std::array<VkCommandPool, SIMULTANEOUS_FRAMES> m_draw_cmd_pool;
    std::array<std::array<VkCommandBuffer, 1>, SIMULTANEOUS_FRAMES> m_draw_cmd;

    virtual std::span<const float> vertex_buffer_data() const { return std::span(vb_data, vb_data + 19); }

public:
    TriangleScene(twogame::vk::DisplayHost* host, twogame::vk::IRenderer* renderer)
        : IScene(host, renderer)
    {
    }
    virtual ~TriangleScene();
    virtual void construct(VkCommandBuffer prepare_commands);
    virtual void handle_event(const SDL_Event& evt, twogame::vk::SceneHost* stage);
    virtual void tick(Uint64 delta_ms, twogame::vk::SceneHost* stage);
    virtual void record_commands(uint32_t frame_number);

    virtual std::span<VkCommandBuffer> draw_commands(uint32_t frame_number, int subpass);
};
class TriangleScene2 : public TriangleScene {
    static constexpr float vb_data2[] = {
        // position
        0.0,
        -0.5,
        0.0,
        0.5,
        0.5,
        0.0,
        -0.5,
        0.5,
        0.0,

        0.0,

        // color
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };

    virtual std::span<const float> vertex_buffer_data() const override { return std::span(vb_data2, vb_data2 + 19); }

public:
    TriangleScene2(twogame::vk::DisplayHost* host, twogame::vk::IRenderer* renderer)
        : TriangleScene(host, renderer)
    {
    }
    virtual ~TriangleScene2() {}
    virtual void handle_event(const SDL_Event& evt, twogame::vk::SceneHost* stage) override;
};

TriangleScene::~TriangleScene()
{
    vmaDestroyBuffer(r_allocator, m_staging_buffer, m_staging_mem);
    vmaDestroyBuffer(r_allocator, m_buffer, m_mem);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(r_host->device(), *it, nullptr);
}

void TriangleScene::construct(VkCommandBuffer prepare_commands)
{
    VkCommandPoolCreateInfo pool_info {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = r_host->queue_family_index(twogame::vk::QueueType::Graphics);

    VkCommandBufferAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateCommandPool(r_host->device(), &pool_info, nullptr, &m_draw_cmd_pool[i]));

        alloc_info.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(r_host->device(), &alloc_info, m_draw_cmd[i].data()));
    }

    VkBufferCreateInfo buffer_info {};
    VmaAllocationCreateInfo mem_createinfo {};
    VmaAllocationInfo mem_info;
    void* mapped_buffer;
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 20 * sizeof(float);
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_DEMAND(vmaCreateBuffer(r_allocator, &buffer_info, &mem_createinfo, &m_buffer, &m_mem, &mem_info));

    VkMemoryPropertyFlags mem_type_flags;
    std::span<const float> data = vertex_buffer_data();
    vmaGetMemoryTypeProperties(r_allocator, mem_info.memoryType, &mem_type_flags);
    if (mem_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        m_staging_buffer = VK_NULL_HANDLE;
        m_staging_mem = VK_NULL_HANDLE;

        vmaMapMemory(r_allocator, m_mem, &mapped_buffer);
        memcpy(mapped_buffer, data.data(), data.size_bytes());
        vmaUnmapMemory(r_allocator, m_mem);
        if ((mem_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            vmaFlushAllocation(r_allocator, m_mem, 0, VK_WHOLE_SIZE);
    } else {
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VK_DEMAND(vmaCreateBuffer(r_allocator, &buffer_info, &mem_createinfo, &m_staging_buffer, &m_staging_mem, nullptr));
        vmaMapMemory(r_allocator, m_staging_mem, &mapped_buffer);
        memcpy(mapped_buffer, data.data(), data.size_bytes());
        vmaUnmapMemory(r_allocator, m_staging_mem);

        VkBufferCopy copy {};
        copy.srcOffset = 0;
        copy.dstOffset = 0;
        copy.size = 19 * sizeof(float);
        vkCmdCopyBuffer(prepare_commands, m_staging_buffer, m_buffer, 1, &copy);

        if (r_host->queue_family_index(twogame::vk::QueueType::Graphics) != r_host->queue_family_index(twogame::vk::QueueType::Transfer)) {
            VkDependencyInfo dep {};
            VkBufferMemoryBarrier2 barrier {};

            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &barrier;
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
            barrier.srcQueueFamilyIndex = r_host->queue_family_index(twogame::vk::QueueType::Transfer);
            barrier.dstQueueFamilyIndex = r_host->queue_family_index(twogame::vk::QueueType::Graphics);
            barrier.buffer = m_buffer;
            barrier.offset = mem_info.offset;
            barrier.size = mem_info.size;
            vkCmdPipelineBarrier2(prepare_commands, &dep);
        }
    }
}

void TriangleScene::handle_event(const SDL_Event& evt, twogame::vk::SceneHost* stage)
{
    if (evt.type == SDL_EVENT_KEY_UP && evt.key.key == SDLK_2) {
        twogame::vk::IScene* next_scene = new TriangleScene2(r_host, r_renderer);
        stage->add(next_scene);
        stage->switch_to(next_scene);
    }
}
void TriangleScene2::handle_event(const SDL_Event& evt, twogame::vk::SceneHost* stage)
{
    if (evt.type == SDL_EVENT_KEY_UP && evt.key.key == SDLK_1) {
        twogame::vk::IScene* next_scene = new TriangleScene(r_host, r_renderer);
        stage->add(next_scene);
        stage->switch_to(next_scene);
    }
}

void TriangleScene::tick(Uint64 delta_ms, twogame::vk::SceneHost* stage)
{
}

void TriangleScene::record_commands(uint32_t frame_number)
{
    vkResetCommandPool(r_host->device(), m_draw_cmd_pool[frame_number % SIMULTANEOUS_FRAMES], 0);

    VkCommandBuffer cmd = m_draw_cmd[frame_number % SIMULTANEOUS_FRAMES][0];
    VkCommandBufferBeginInfo begin_info {};
    VkCommandBufferInheritanceInfo inherit_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inherit_info;
    inherit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inherit_info.renderPass = r_renderer->render_pass();
    inherit_info.subpass = 0;
    VK_DEMAND(vkBeginCommandBuffer(cmd, &begin_info));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r_renderer->pipeline(0));

    VkExtent2D swapchain_extent = r_renderer->swapchain_extent();
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

    std::array<VkBuffer, 2> buffers = { m_buffer, m_buffer };
    std::array<VkDeviceSize, 2> buffer_offs = { 0, 10 * sizeof(float) };
    vkCmdBindVertexBuffers(cmd, 0, buffers.size(), buffers.data(), buffer_offs.data());
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkEndCommandBuffer(cmd);
}

std::span<VkCommandBuffer> TriangleScene::draw_commands(uint32_t frame_number, int subpass)
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
    twogame::vk::DisplayHost* host;
    twogame::vk::SimpleForwardRenderer* renderer;
    twogame::vk::SceneHost* stage;

    AppState()
        : host(nullptr)
        , renderer(nullptr)
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
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif

    twogame::init();
#ifdef DEBUG_BUILD
    twogame::init_filesystem_debug(argv[0]);
#else
    twogame::init_filesystem(argv[0], SHORT_ORG_NAME, SHORT_APP_NAME);
#endif

    try {
        appstate.host = new twogame::vk::DisplayHost;
        appstate.renderer = new twogame::vk::SimpleForwardRenderer(appstate.host);
        appstate.stage = new twogame::vk::SceneHost(appstate.host, new TriangleScene(appstate.host, appstate.renderer));
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
    appstate->stage->tick();
    appstate->host->draw_frame(appstate->renderer, appstate->stage);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (appstate->stage)
        delete appstate->stage;
    if (appstate->renderer)
        delete appstate->renderer;
    if (appstate->host)
        delete appstate->host;
    twogame::deinit();
}
