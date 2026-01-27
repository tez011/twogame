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
    static constexpr float data[] = {
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

    VkBuffer m_buffer, m_staging_buffer;
    VmaAllocation m_mem, m_staging_mem;

    // these probably belong in a scene manager of some kind, so we can reuse them. but for now there is only one scene so
    // can we call this a "stage" / "backstage" / etc.
    // even if we have unified memory, it's not clear we can just skip this.
    // I think we need two command buffers, one for each frame in flight. That means we need to know the current frame number too.
    // But it must be passed in to draw() because this will be running in a separate thread someday
    VkCommandPool m_staging_cmd_pool;
    std::array<VkCommandBuffer, SIMULTANEOUS_FRAMES> m_staging_cmd;
    std::array<VkFence, SIMULTANEOUS_FRAMES> m_fence_staging;

public:
    TriangleScene(twogame::vk::DisplayHost* host)
        : IScene(host)
    {
        VkCommandPoolCreateInfo pool_info {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_index(twogame::vk::QueueType::Transfer);
        VK_DEMAND(vkCreateCommandPool(device(), &pool_info, nullptr, &m_staging_cmd_pool));

        VkCommandBufferAllocateInfo alloc_info {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = m_staging_cmd_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = m_staging_cmd.size();
        VK_DEMAND(vkAllocateCommandBuffers(device(), &alloc_info, m_staging_cmd.data()));

        VkFenceCreateInfo fence_info {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto it = m_fence_staging.begin(); it != m_fence_staging.end(); ++it)
            VK_DEMAND(vkCreateFence(device(), &fence_info, nullptr, &*it));

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
        VK_DEMAND(vmaCreateBuffer(allocator(), &buffer_info, &mem_createinfo, &m_buffer, &m_mem, &mem_info));

        VkCommandBufferBeginInfo begin_info {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_DEMAND(vkBeginCommandBuffer(m_staging_cmd[0], &begin_info));

        VkMemoryPropertyFlags mem_type_flags;
        vmaGetMemoryTypeProperties(allocator(), mem_info.memoryType, &mem_type_flags);
        if (mem_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            m_staging_buffer = VK_NULL_HANDLE;
            m_staging_mem = VK_NULL_HANDLE;

            vmaMapMemory(allocator(), m_mem, &mapped_buffer);
            memcpy(mapped_buffer, data, 19 * sizeof(float));
            vmaUnmapMemory(allocator(), m_mem);
            if ((mem_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
                vmaFlushAllocation(allocator(), m_mem, 0, VK_WHOLE_SIZE);
        } else {
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            mem_createinfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            VK_DEMAND(vmaCreateBuffer(allocator(), &buffer_info, &mem_createinfo, &m_staging_buffer, &m_staging_mem, nullptr));
            vmaMapMemory(allocator(), m_staging_mem, &mapped_buffer);
            memcpy(mapped_buffer, data, 19 * sizeof(float));
            vmaUnmapMemory(allocator(), m_staging_mem);

            VkBufferCopy copy {};
            copy.srcOffset = 0;
            copy.dstOffset = 0;
            copy.size = 19 * sizeof(float);
            vkCmdCopyBuffer(m_staging_cmd[0], m_staging_buffer, m_buffer, 1, &copy);

            if (queue_family_index(twogame::vk::QueueType::Graphics) != queue_family_index(twogame::vk::QueueType::Transfer)) {
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
                barrier.srcQueueFamilyIndex = queue_family_index(twogame::vk::QueueType::Transfer);
                barrier.dstQueueFamilyIndex = queue_family_index(twogame::vk::QueueType::Graphics);
                barrier.buffer = m_buffer;
                barrier.offset = mem_info.offset;
                barrier.size = mem_info.size;
                vkCmdPipelineBarrier2(m_staging_cmd[0], &dep);
            }
        }
        VK_DEMAND(vkEndCommandBuffer(m_staging_cmd[0]));

        VkQueue scene_xfer_queue;
        VkSubmitInfo submit {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_staging_cmd[0];
        vkGetDeviceQueue(device(), queue_family_index(twogame::vk::QueueType::Transfer), 0, &scene_xfer_queue); // NOT SAFE if different thread
        vkResetFences(device(), 1, &m_fence_staging[0]);
        VK_DEMAND(vkQueueSubmit(scene_xfer_queue, 1, &submit, m_fence_staging[0]));
    }
    ~TriangleScene()
    {
        vkDeviceWaitIdle(device());
        vkDestroyBuffer(device(), m_staging_buffer, nullptr);
        vmaFreeMemory(allocator(), m_staging_mem);
        vkDestroyBuffer(device(), m_buffer, nullptr);
        vmaFreeMemory(allocator(), m_mem);

        for (auto it = m_fence_staging.begin(); it != m_fence_staging.end(); ++it)
            vkDestroyFence(device(), *it, nullptr);
        vkDestroyCommandPool(device(), m_staging_cmd_pool, nullptr);
    }

    virtual void record_draw_calls(VkCommandBuffer commands, uint32_t frame_number)
    {
        VkResult res = vkWaitForFences(device(), m_fence_staging.size(), m_fence_staging.data(), VK_TRUE, 0);
        if (res == VK_TIMEOUT) {
            return; // staged data not ready
        } else if (res == VK_SUCCESS) {
            vkDestroyBuffer(device(), m_staging_buffer, nullptr);
            vmaFreeMemory(allocator(), m_staging_mem);
            m_staging_buffer = VK_NULL_HANDLE;
            m_staging_mem = VK_NULL_HANDLE;
        } else {
            VK_DEMAND(res);
        }

        std::array<VkBuffer, 2> buffers = { m_buffer, m_buffer };
        std::array<VkDeviceSize, 2> buffer_offs = { 0, 10 * sizeof(float) };
        vkCmdBindVertexBuffers(commands, 0, buffers.size(), buffers.data(), buffer_offs.data());
        vkCmdDraw(commands, 3, 1, 0, 0);
    }
};

// fields that could be part of appstate
struct AppState {
    twogame::vk::DisplayHost* host;
    twogame::vk::SimpleForwardRenderer* renderer;
    twogame::vk::IScene* scene;

    AppState()
        : host(nullptr)
        , renderer(nullptr)
        , scene(nullptr)
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
        appstate.scene = new TriangleScene(appstate.host);
    } catch (...) {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);

    switch (evt->type) {
    default:
        return appstate->host->handle_event(evt);
    }
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    appstate->host->draw_frame(appstate->renderer, appstate->scene);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (appstate->scene)
        delete appstate->scene;
    if (appstate->renderer)
        delete appstate->renderer;
    if (appstate->host)
        delete appstate->host;
    twogame::deinit();
}
