#include "scene.h"
#include <cinttypes>
#include <set>

namespace twogame {

std::unique_ptr<SceneHost> SceneHost::s_self;

void SceneHost::StagingBuffer::copy_buffer(VkBuffer dst, VkDeviceSize dst_size, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkBufferMemoryBarrier2& barrier = m_buffer_memory_barriers.emplace_back();
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex = DisplayHost::queue_family_index_dma();
    barrier.dstQueueFamilyIndex = DisplayHost::queue_family_index();
    barrier.buffer = dst;
    barrier.offset = 0;
    barrier.size = dst_size;

    auto& copy = m_buffer_copies.emplace_back();
    copy.second = std::vector(regions.begin(), regions.end());
    copy.first.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copy.first.srcBuffer = m_src_buffer;
    copy.first.dstBuffer = dst;
    copy.first.regionCount = copy.second.size();
    copy.first.pRegions = copy.second.data();
}

void SceneHost::StagingBuffer::copy_image(VkImage dst, VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout)
{
    VkImageMemoryBarrier2 barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dst;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = info.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = info.arrayLayers;
    m_image_memory_barriers[0].push_back(barrier);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.newLayout = final_layout;
    barrier.srcQueueFamilyIndex = DisplayHost::queue_family_index_dma();
    barrier.dstQueueFamilyIndex = DisplayHost::queue_family_index();
    m_image_memory_barriers[1].push_back(barrier);

    auto& copy = m_image_copies.emplace_back();
    copy.second = std::vector(copies.begin(), copies.end());
    copy.first.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    copy.first.srcBuffer = m_src_buffer;
    copy.first.dstImage = dst;
    copy.first.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy.first.regionCount = copy.second.size();
    copy.first.pRegions = copy.second.data();
}

void SceneHost::StagingBuffer::finalize()
{
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(m_xfer_commands, &begin_info));

    VkDependencyInfo dep {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = m_image_memory_barriers[0].size();
    dep.pImageMemoryBarriers = m_image_memory_barriers[0].data();
    vkCmdPipelineBarrier2(m_xfer_commands, &dep);
    for (auto it = m_buffer_copies.begin(); it != m_buffer_copies.end(); ++it)
        vkCmdCopyBuffer2(m_xfer_commands, &it->first);
    for (auto it = m_image_copies.begin(); it != m_image_copies.end(); ++it)
        vkCmdCopyBufferToImage2(m_xfer_commands, &it->first);

    dep.bufferMemoryBarrierCount = m_buffer_memory_barriers.size();
    dep.pBufferMemoryBarriers = m_buffer_memory_barriers.data();
    dep.imageMemoryBarrierCount = m_image_memory_barriers[1].size();
    dep.pImageMemoryBarriers = m_image_memory_barriers[1].data();
    vkCmdPipelineBarrier2(m_xfer_commands, &dep);
    VK_DEMAND(vkEndCommandBuffer(m_xfer_commands));

    if (m_acquire_commands != VK_NULL_HANDLE) {
        VK_DEMAND(vkBeginCommandBuffer(m_acquire_commands, &begin_info));
        vkCmdPipelineBarrier2(m_acquire_commands, &dep);
        VK_DEMAND(vkEndCommandBuffer(m_acquire_commands));
    }

    m_buffer_copies.clear();
    m_image_copies.clear();
    m_buffer_memory_barriers.clear();
    m_image_memory_barriers[0].clear();
    m_image_memory_barriers[1].clear();
}

SceneHost::SceneHost(IRenderer* renderer, IScene* initial)
    : m_active_scene(nullptr)
    , m_requested_scene(nullptr)
    , m_active(true)
    , m_renderer(renderer)
{
    std::array<VkSemaphore, BUILDER_THREAD_COUNT> builder_sem;
    VkSemaphoreCreateInfo sem_createinfo {};
    VkSemaphoreTypeCreateInfo sem_typeinfo {};
    sem_createinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    vkGetDeviceQueue(DisplayHost::device(), DisplayHost::queue_family_index(), 0, &m_graphics_queue);
    vkGetDeviceQueue(DisplayHost::device(), DisplayHost::queue_family_index_dma(), 0, &m_transfer_queue);
    if (m_graphics_queue == m_transfer_queue) {
        builder_sem.fill(VK_NULL_HANDLE);
    } else {
        for (size_t i = 0; i < BUILDER_THREAD_COUNT; i++)
            VK_DEMAND(vkCreateSemaphore(DisplayHost::device(), &sem_createinfo, nullptr, &builder_sem[i]));
    }

    sem_createinfo.pNext = &sem_typeinfo;
    sem_typeinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_typeinfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_typeinfo.initialValue = 0;
    VK_DEMAND(vkCreateSemaphore(DisplayHost::device(), &sem_createinfo, nullptr, &m_timeline));

    VkCommandPoolCreateInfo pool_createinfo {};
    pool_createinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_createinfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_createinfo.queueFamilyIndex = DisplayHost::queue_family_index_dma();
    VK_DEMAND(vkCreateCommandPool(DisplayHost::device(), &pool_createinfo, nullptr, &m_xfer_command_pool));
    pool_createinfo.queueFamilyIndex = DisplayHost::queue_family_index();
    VK_DEMAND(vkCreateCommandPool(DisplayHost::device(), &pool_createinfo, nullptr, &m_acquire_command_pool));

    std::array<std::array<VkCommandBuffer, BUILDER_THREAD_COUNT>, 2> builder_commands;
    VkCommandBufferAllocateInfo cmd_allocinfo {};
    cmd_allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_allocinfo.commandPool = m_xfer_command_pool;
    cmd_allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_allocinfo.commandBufferCount = BUILDER_THREAD_COUNT;
    VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::device(), &cmd_allocinfo, builder_commands[0].data()));
    if (m_graphics_queue == m_transfer_queue) {
        builder_commands[1].fill(VK_NULL_HANDLE);
    } else {
        cmd_allocinfo.commandPool = m_acquire_command_pool;
        VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::device(), &cmd_allocinfo, builder_commands[1].data()));
    }

    VkBufferCreateInfo staging_createinfo {};
    VmaAllocationCreateInfo staging_allocinfo {};
    staging_createinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_createinfo.size = STAGING_BUFFER_SIZE;
    staging_createinfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_createinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    staging_allocinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    staging_allocinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    for (size_t i = 0; i < BUILDER_THREAD_COUNT; i++) {
        VmaAllocationInfo staging_meminfo;
        VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &staging_createinfo, &staging_allocinfo,
            &m_staging_buffers[i].m_src_buffer, &m_staging_buffers[i].m_src_mem, &staging_meminfo));

        m_staging_buffers[i].m_src_data = std::span(static_cast<std::byte*>(staging_meminfo.pMappedData), STAGING_BUFFER_SIZE);
        m_staging_buffers[i].m_xfer_commands = builder_commands[0][i];
        m_staging_buffers[i].m_acquire_commands = builder_commands[1][i];
        m_staging_buffers[i].m_post_xfer = builder_sem[i];
    }

    // Prepare the initial scene in-line.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint64_t pass = 0;
    bool complete;
    do {
        complete = initial->construct(m_renderer.get(), m_staging_buffers[0], pass, pass + 1);
        m_staging_buffers[0].finalize();
        pass++;

        VkSubmitInfo submit {};
        VkTimelineSemaphoreSubmitInfo timeline_info {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &m_staging_buffers[0].m_xfer_commands;
        submit.signalSemaphoreCount = 1;
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &pass;
        if (m_graphics_queue == m_transfer_queue) {
            submit.pNext = &timeline_info;
            submit.pSignalSemaphores = &m_timeline;
            VK_DEMAND(vkQueueSubmit(m_transfer_queue, 1, &submit, VK_NULL_HANDLE));
        } else {
            submit.pSignalSemaphores = &m_staging_buffers[0].m_post_xfer;
            VK_DEMAND(vkQueueSubmit(m_transfer_queue, 1, &submit, VK_NULL_HANDLE));

            submit.pNext = &timeline_info;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &m_staging_buffers[0].m_post_xfer;
            submit.pWaitDstStageMask = &wait_stage;
            submit.pCommandBuffers = &m_staging_buffers[0].m_acquire_commands;
            submit.pSignalSemaphores = &m_timeline;
            VK_DEMAND(vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE));
        }

        VkSemaphoreWaitInfo wait_info {};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &m_timeline;
        wait_info.pValues = &pass;

        VK_DEMAND(vkWaitSemaphores(DisplayHost::device(), &wait_info, UINT64_MAX));
    } while (complete == false);
    m_scenes[initial] = pass;
    m_requested_scene = initial;
    m_max_ticket.store(pass + 1, std::memory_order_relaxed);
    initial->record_commands(m_renderer.get(), 0);

    m_scene_host = std::thread(&SceneHost::scene_loop, this);
    for (size_t i = 0; i < BUILDER_THREAD_COUNT; i++)
        m_builders[i] = std::thread(&SceneHost::builder_loop, this, i);
}

SceneHost::~SceneHost()
{
    BQData terminate_payload { nullptr, false };
    m_active = false;
    DisplayHost::s_self->m_frame_number = UINT32_MAX;
    DisplayHost::s_self->m_frame_number.notify_all();
    for (size_t i = 0; i < 2 * m_builders.size(); i++)
        m_builder_queue.push(terminate_payload);
    for (auto it = m_builders.begin(); it != m_builders.end(); ++it)
        it->join();
    m_scene_host.join();

    vkDeviceWaitIdle(DisplayHost::device());
    for (auto it = m_scenes.begin(); it != m_scenes.end(); ++it)
        delete it->first;

    vkDestroyCommandPool(DisplayHost::device(), m_xfer_command_pool, nullptr);
    vkDestroyCommandPool(DisplayHost::device(), m_acquire_command_pool, nullptr);
    vkDestroySemaphore(DisplayHost::device(), m_timeline, nullptr);
}

void SceneHost::init(IRenderer* renderer, IScene* initial)
{
    if (s_self) {
        s_self->m_renderer.reset(renderer);
        s_self->m_requested_scene = initial;
        prepare(initial);
    } else {
        s_self = std::unique_ptr<SceneHost> { new SceneHost(renderer, initial) };
    }
}

void SceneHost::drop()
{
    SDL_assert(s_self);
    s_self.reset();
}

void SceneHost::scene_loop()
{
    const DisplayHost& display = DisplayHost::instance();
    std::array<uint64_t, 2> frame_time = { SDL_GetTicks(), 0 };
    while (m_active) {
        uint32_t frame_number = m_frame_number.load(std::memory_order_relaxed) + 1;
        uint64_t timeline_value = 0;
        VK_DEMAND(vkGetSemaphoreCounterValue(display.m_device, m_timeline, &timeline_value));

        // Wait for the last frame's resources to be free before we record commands for the next frame.
        uint32_t render_frame_number = display.m_frame_number.load(std::memory_order_acquire);
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "scene  thread: F%u WAITING (H%u)", frame_number, render_frame_number);
        while ((render_frame_number = display.m_frame_number.load(std::memory_order_acquire)) < frame_number)
            display.m_frame_number.wait(render_frame_number, std::memory_order_relaxed);
        if (m_active == false)
            break;
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "scene  thread: F%u BEGIN", frame_number);

        IScene* scene = m_active_scene.load(std::memory_order_acquire);
        if (m_requested_scene && m_scenes[m_requested_scene] <= timeline_value) {
            // The requested scene is ready. Execute that one.
            scene = m_requested_scene;
        }
        if (scene) {
            // Execute the current scene, and update the frame number and notify the render thread when commands are recorded
            SDL_Event evt;
            frame_time[1] = SDL_GetTicks();
            while (m_event_queue.try_pop(evt))
                scene->handle_event(evt, this);
            scene->tick(frame_time[1], frame_time[1] - frame_time[0], this);
            scene->record_commands(m_renderer.get(), frame_number);
            frame_time[0] = frame_time[1];

            if (scene == m_requested_scene) {
                IScene* last_scene = m_active_scene.exchange(scene, std::memory_order_release);
                m_requested_scene = nullptr;
                if (last_scene)
                    m_purge_queue.emplace(last_scene, frame_number + 100);
            }
        }
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "scene  thread: F%u END", frame_number);
        m_frame_number.store(frame_number, std::memory_order_release);
        m_frame_number.notify_all();

        RQData job;
        while (m_return_queue.try_pop(job)) {
            m_scenes[job.scene] = job.ticket;
        }

        if (m_purge_queue.empty() == false) {
            int32_t frames_before_purge = static_cast<int32_t>(m_purge_queue.front().second - frame_number);
            if (frames_before_purge <= 0) {
                IScene* purge_scene = m_purge_queue.front().first;
                m_purge_queue.pop();
                if (purge_scene != m_active_scene && purge_scene != m_requested_scene) {
                    BQData payload = { purge_scene, false };
                    m_scenes.erase(purge_scene);
                    m_builder_queue.push(payload);
                }
            }
        }
    }
}

void SceneHost::builder_loop(int thread_id)
{
    while (true) {
        BQData build_job;
        m_builder_queue.pop(build_job);
        if (build_job.scene == nullptr && build_job.bringup == false) {
            vmaDestroyBuffer(DisplayHost::allocator(), m_staging_buffers[thread_id].m_src_buffer, m_staging_buffers[thread_id].m_src_mem);
            vkDestroySemaphore(DisplayHost::device(), m_staging_buffers[thread_id].m_post_xfer, nullptr);
            return;
        }

        if (build_job.bringup) {
            VkSemaphoreWaitInfo wait_info {};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &m_timeline;

            RQData job;
            bool complete;
            int pass = 0;
            job.scene = build_job.scene;
            job.commands = &m_staging_buffers[thread_id];
            do {
                job.ticket = m_max_ticket.fetch_add(1, std::memory_order_relaxed);
                complete = job.scene->construct(m_renderer.get(), m_staging_buffers[thread_id], pass++, job.ticket);
                m_staging_buffers[thread_id].finalize();
                m_render_queue.push(job);
                SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup=%p", job.scene, job.ticket, job.commands);

                // Because the builder thread blocks until the command buffer we just submitted is complete, we don't need any GPU waiting.
                wait_info.pValues = &job.ticket;
                VK_DEMAND(vkWaitSemaphores(DisplayHost::device(), &wait_info, UINT64_MAX));
            } while (complete == false);
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup complete", job.scene, job.ticket);
            m_return_queue.push(job);
        } else {
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p teardown", build_job.scene);
            delete build_job.scene;
        }
    }
}

bool SceneHost::prepare(IScene* scene)
{
    BQData job { scene, true };
    if (s_self->m_scenes.find(scene) == s_self->m_scenes.end())
        return s_self->m_builder_queue.try_push(job);
    else
        return true;
}

void SceneHost::set_next_scene(IScene* scene)
{
    s_self->m_requested_scene = scene;
}

void SceneHost::wait_frame(uint32_t frame_number)
{
    uint32_t actual_frame;
    SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u WAIT FOR COMMANDS (F%u)", frame_number, s_self->m_frame_number.load());
    while ((actual_frame = s_self->m_frame_number.load(std::memory_order_acquire)) < frame_number)
        s_self->m_frame_number.wait(actual_frame, std::memory_order_relaxed);

    SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u COMMANDS READY", frame_number);
}

void SceneHost::push_event(SDL_Event* evt)
{
    s_self->m_event_queue.push(*evt);
}

void SceneHost::submit_transfers()
{
    RQData job;
    uint64_t max_ticket = 0, num_commands = 0;
    std::array<VkSubmitInfo, 8> xfer_commands, acquire_commands;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo timeline_info {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.signalSemaphoreValueCount = 1;

    while (num_commands < xfer_commands.size() && s_self->m_render_queue.try_pop(job)) {
        xfer_commands[num_commands].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        xfer_commands[num_commands].commandBufferCount = 1;
        xfer_commands[num_commands].pCommandBuffers = &job.commands->m_xfer_commands;
        xfer_commands[num_commands].signalSemaphoreCount = 1;
        if (s_self->m_graphics_queue == s_self->m_transfer_queue) {
            xfer_commands[num_commands].pNext = &timeline_info;
            xfer_commands[num_commands].pSignalSemaphores = &s_self->m_timeline;
        } else {
            xfer_commands[num_commands].pSignalSemaphores = &job.commands->m_post_xfer;
            acquire_commands[num_commands].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            acquire_commands[num_commands].pNext = &timeline_info;
            acquire_commands[num_commands].waitSemaphoreCount = 1;
            acquire_commands[num_commands].pWaitSemaphores = &job.commands->m_post_xfer;
            acquire_commands[num_commands].pWaitDstStageMask = &wait_stage;
            acquire_commands[num_commands].commandBufferCount = 1;
            acquire_commands[num_commands].pCommandBuffers = &job.commands->m_acquire_commands;
            acquire_commands[num_commands].signalSemaphoreCount = 1;
            acquire_commands[num_commands].pSignalSemaphores = &s_self->m_timeline;
        }

        max_ticket = std::max(max_ticket, job.ticket);
        num_commands++;
    }

    if (num_commands > 0) {
        timeline_info.pSignalSemaphoreValues = &max_ticket;
        VK_DEMAND(vkQueueSubmit(s_self->m_transfer_queue, num_commands, xfer_commands.data(), VK_NULL_HANDLE));
        if (s_self->m_graphics_queue != s_self->m_transfer_queue)
            VK_DEMAND(vkQueueSubmit(s_self->m_graphics_queue, num_commands, acquire_commands.data(), VK_NULL_HANDLE));
    }
}

void SceneHost::execute_draws(VkCommandBuffer container, uint32_t frame_number, int subpass)
{
    IScene* active_scene = s_self->m_active_scene.load(std::memory_order_acquire);
    if (active_scene) {
        std::span<VkCommandBuffer> commands = active_scene->draw_commands(frame_number, subpass);
        if (commands.size() > 0)
            vkCmdExecuteCommands(container, commands.size(), commands.data());
    }
}

}
