#include "scene.h"
#include <cinttypes>
#include <set>

namespace twogame {

SceneHost::SceneHost(IRenderer* renderer, IScene* initial)
    : m_active_scene(nullptr)
    , m_requested_scene(nullptr)
    , m_active(true)
    , r_renderer(renderer)
{
    DisplayHost& host = renderer->r_host;
    VkSemaphoreCreateInfo sem_createinfo {};
    VkSemaphoreTypeCreateInfo sem_typeinfo {};
    sem_createinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_createinfo.pNext = &sem_typeinfo;
    sem_typeinfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sem_typeinfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sem_typeinfo.initialValue = 0;
    VK_DEMAND(vkCreateSemaphore(host.m_device, &sem_createinfo, nullptr, &m_timeline));

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
        VK_DEMAND(vmaCreateBuffer(host.m_allocator, &staging_createinfo, &staging_allocinfo, &m_staging_buffers[i].buffer, &m_staging_buffers[i].mem, &staging_meminfo));
        m_staging_buffers[i].ptr = static_cast<unsigned char*>(staging_meminfo.pMappedData);
    }

    VkCommandPoolCreateInfo pool_createinfo {};
    pool_createinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_createinfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_createinfo.queueFamilyIndex = host.queue_family_index(QueueType::Transfer);
    VK_DEMAND(vkCreateCommandPool(host.m_device, &pool_createinfo, nullptr, &m_command_pool));
    vkGetDeviceQueue(host.m_device, host.queue_family_index(QueueType::Transfer), 0, &m_transfer_queue);

    VkCommandBufferAllocateInfo cmd_allocinfo {};
    std::array<VkCommandBuffer, 8> spare_commands;
    cmd_allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_allocinfo.commandPool = m_command_pool;
    cmd_allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_allocinfo.commandBufferCount = spare_commands.size();
    VK_DEMAND(vkAllocateCommandBuffers(host.m_device, &cmd_allocinfo, spare_commands.data()));
    for (auto it = spare_commands.begin() + 1; it != spare_commands.end(); ++it)
        m_spare_commands.push(*it);

    // Prepare the initial scene in-line.
    bool prep_complete = false;
    uint64_t pass = 0;
    while (!prep_complete) {
        VkCommandBufferBeginInfo begin_info {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_DEMAND(vkBeginCommandBuffer(spare_commands[0], &begin_info));
        prep_complete = initial->construct(r_renderer, spare_commands[0], pass++, m_staging_buffers[0].buffer, m_staging_buffers[0].ptr);
        vkEndCommandBuffer(spare_commands[0]);

        VkSubmitInfo submit {};
        VkTimelineSemaphoreSubmitInfo timeline_info {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline_info;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &spare_commands[0];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &m_timeline;
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &pass;
        VK_DEMAND(vkQueueSubmit(m_transfer_queue, 1, &submit, VK_NULL_HANDLE));

        VkSemaphoreWaitInfo wait_info {};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &m_timeline;
        wait_info.pValues = &pass;
        VK_DEMAND(vkWaitSemaphores(host.m_device, &wait_info, UINT64_MAX));
    }
    vkResetCommandBuffer(spare_commands[0], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    m_spare_commands.push(spare_commands[0]);
    m_scenes[initial] = pass;
    m_requested_scene = initial;
    m_max_ticket.store(pass + 1, std::memory_order_relaxed);

    m_scene_host = std::thread(&SceneHost::scene_loop, this);
    for (size_t i = 0; i < BUILDER_THREAD_COUNT; i++)
        m_builders[i] = std::thread(&SceneHost::builder_loop, this, i);
}

SceneHost::~SceneHost()
{
    VkDevice device = r_renderer->r_host.m_device;
    QData terminate_payload { nullptr, VK_NULL_HANDLE, 0 };
    m_active = false;
    r_renderer->r_host.m_frame_number = UINT32_MAX;
    r_renderer->r_host.m_frame_number.notify_all();
    for (size_t i = 0; i < 2 * m_builders.size(); i++)
        m_builder_queue.push(terminate_payload);
    for (auto it = m_builders.begin(); it != m_builders.end(); ++it)
        it->join();
    m_scene_host.join();

    vkDeviceWaitIdle(device);
    for (auto it = m_scenes.begin(); it != m_scenes.end(); ++it)
        delete it->first;

    vkDestroyCommandPool(device, m_command_pool, nullptr);
    vkDestroySemaphore(device, m_timeline, nullptr);
}

void SceneHost::scene_loop()
{
    DisplayHost& host = r_renderer->r_host;
    std::array<uint64_t, 2> frame_time = { SDL_GetTicks(), 0 };
    while (m_active) {
        uint32_t frame_number = m_frame_number.load(std::memory_order_relaxed) + 1;
        uint64_t timeline_value = 0;
        VK_DEMAND(vkGetSemaphoreCounterValue(host.m_device, m_timeline, &timeline_value));

        // Wait for the last frame's resources to be free before we record commands for the next frame.
        uint32_t render_frame_number = host.m_frame_number.load(std::memory_order_acquire);
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "scene  thread: F%u PENDING (H%u)", frame_number, render_frame_number);
        while ((render_frame_number = host.m_frame_number.load(std::memory_order_acquire)) < frame_number)
            host.m_frame_number.wait(render_frame_number, std::memory_order_relaxed);
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
            scene->record_commands(r_renderer, frame_number);
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

        QData job;
        while (m_return_queue.try_pop(job)) {
            vkResetCommandBuffer(job.commands, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
            m_spare_commands.push(job.commands);
            m_scenes[job.scene] = job.ticket;
        }

        if (m_purge_queue.empty() == false) {
            int32_t frames_before_purge = static_cast<int32_t>(m_purge_queue.front().second - frame_number);
            if (frames_before_purge <= 0) {
                IScene* purge_scene = m_purge_queue.front().first;
                QData payload = { purge_scene, VK_NULL_HANDLE, 0 };
                m_scenes.erase(purge_scene);
                m_builder_queue.push(payload);
                m_purge_queue.pop();
            }
        }
    }
}

void SceneHost::builder_loop(int thread_id)
{
    while (true) {
        QData job;
        m_builder_queue.pop(job);
        if (job.scene == nullptr && job.ticket == 0) {
            vmaDestroyBuffer(r_renderer->r_host.m_allocator, m_staging_buffers[thread_id].buffer, m_staging_buffers[thread_id].mem);
            return;
        }

        if (job.commands == VK_NULL_HANDLE) {
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p teardown", job.scene);
            delete job.scene;
        } else {
            VkSemaphoreWaitInfo wait_info {};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &m_timeline;

            bool complete = false;
            int pass = 0;
            while (!complete) {
                VkCommandBufferBeginInfo begin_info {};
                begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VK_DEMAND(vkBeginCommandBuffer(job.commands, &begin_info));
                complete = job.scene->construct(r_renderer, job.commands, pass++, m_staging_buffers[thread_id].buffer, m_staging_buffers[thread_id].ptr);
                vkEndCommandBuffer(job.commands);

                job.ticket = m_max_ticket.fetch_add(1, std::memory_order_relaxed);
                SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup=%p", job.scene, job.ticket, job.commands);
                m_render_queue.push(job);

                // Because the builder thread blocks until the command buffer we just submitted is complete, we don't need any GPU waiting.
                wait_info.pValues = &job.ticket;
                VK_DEMAND(vkWaitSemaphores(r_renderer->device(), &wait_info, UINT64_MAX));
            }
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup complete", job.scene, job.ticket);
            m_return_queue.push(job);
        }
    }
}

bool SceneHost::prepare(IScene* scene)
{
    QData job;
    job.scene = scene;
    job.commands = m_spare_commands.top();
    if (m_builder_queue.try_push(job)) {
        m_spare_commands.pop();
        return true;
    } else {
        return false;
    }
}

void SceneHost::set_next_scene(IScene* scene)
{
    m_requested_scene = scene;
}

void SceneHost::wait_frame(uint32_t frame_number)
{
    uint32_t actual_frame;
    SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u WAIT FOR COMMANDS (F%u)", frame_number, m_frame_number.load());
    while ((actual_frame = m_frame_number.load(std::memory_order_acquire)) < frame_number)
        m_frame_number.wait(actual_frame, std::memory_order_relaxed);

    SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u COMMANDS READY", frame_number);
}

void SceneHost::push_event(SDL_Event* evt)
{
    m_event_queue.push(*evt);
}

void SceneHost::tick()
{
    QData job;
    uint64_t max_ticket = 0, num_commands = 0;
    std::array<VkCommandBuffer, 8> all_commands;
    while (num_commands < all_commands.size() && m_render_queue.try_pop(job)) {
        all_commands[num_commands++] = job.commands;
        max_ticket = std::max(max_ticket, job.ticket);
    }
    if (num_commands > 0) {
        VkSubmitInfo submit {};
        VkTimelineSemaphoreSubmitInfo timeline_info {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline_info;
        submit.commandBufferCount = num_commands;
        submit.pCommandBuffers = all_commands.data();
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &m_timeline;
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &max_ticket;
        VK_DEMAND(vkQueueSubmit(m_transfer_queue, 1, &submit, VK_NULL_HANDLE));
    }
}

void SceneHost::execute_draws(VkCommandBuffer container, uint32_t frame_number, int subpass)
{
    IScene* active_scene = m_active_scene.load(std::memory_order_acquire);
    if (active_scene) {
        std::span<VkCommandBuffer> commands = active_scene->draw_commands(frame_number, subpass);
        if (commands.size() > 0)
            vkCmdExecuteCommands(container, commands.size(), commands.data());
    }
}

}
