#include <cinttypes>
#include <set>
#include "render.h"

namespace twogame::vk {

SceneHost::SceneHost(IRenderer* renderer, IScene* initial)
    : m_active_scene(nullptr)
    , m_requested_scene(nullptr)
    , m_max_ticket(1)
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

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(spare_commands[0], &begin_info));
    initial->construct(r_renderer, spare_commands[0]);
    vkEndCommandBuffer(spare_commands[0]);
    m_requested_scene = &m_prepare_scenes.emplace_back(initial, 1, spare_commands[0]);

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
    timeline_info.pSignalSemaphoreValues = &m_max_ticket;
    VK_DEMAND(vkQueueSubmit(m_transfer_queue, 1, &submit, VK_NULL_HANDLE));
    m_max_ticket++;

    m_scene_host = std::thread(&SceneHost::scene_loop, this);
    for (auto it = m_builders.begin(); it != m_builders.end(); ++it)
        *it = std::thread(&SceneHost::worker_loop, this);
}

SceneHost::~SceneHost()
{
    VkDevice device = r_renderer->r_host.m_device;
    QData terminate_payload { nullptr, 0, VK_NULL_HANDLE };
    m_active = false;
    r_renderer->r_host.m_frame_number = UINT32_MAX;
    r_renderer->r_host.m_frame_number.notify_all();
    for (size_t i = 0; i < 2 * m_builders.size(); i++)
        m_worker_queue.push(terminate_payload);

    std::set<IScene*> deleted_scenes;
    if (m_requested_scene && m_requested_scene->scene)
        deleted_scenes.insert(m_requested_scene->scene);
    if (m_active_scene)
        deleted_scenes.insert(m_active_scene);
    for (auto it = m_prepare_scenes.begin(); it != m_prepare_scenes.end(); ++it)
        deleted_scenes.insert(it->scene);
    for (auto it = m_builders.begin(); it != m_builders.end(); ++it)
        it->join();
    m_scene_host.join();

    vkDeviceWaitIdle(device);
    for (auto it = deleted_scenes.begin(); it != deleted_scenes.end(); ++it)
        delete *it;

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
        if (m_requested_scene && m_requested_scene->ticket <= timeline_value) {
            // The requested scene is ready. Execute that one.
            scene = m_requested_scene->scene;
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
        }
        if (m_requested_scene && scene == m_requested_scene->scene) {
            // If we executed the requested scene, it should now be active.
            IScene* last_scene = m_active_scene.exchange(scene, std::memory_order_release);
            m_requested_scene = nullptr;
            if (last_scene)
                m_purge_queue.emplace(last_scene, frame_number + 100);
        }
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "scene  thread: F%u END", frame_number);
        m_frame_number.store(frame_number, std::memory_order_release);
        m_frame_number.notify_all();

        for (auto it = m_prepare_scenes.begin(); it != m_prepare_scenes.end(); ++it) {
            if (it->ticket <= timeline_value && it->commands != VK_NULL_HANDLE) {
                vkResetCommandBuffer(it->commands, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
                m_spare_commands.push(it->commands);
                it->scene->cleanup_staging();
                it->commands = VK_NULL_HANDLE;
            }
        }
        while (m_purge_queue.empty() == false && m_purge_queue.front().second < frame_number) {
            IScene* purge_scene = m_purge_queue.front().first;
            QData payload = { purge_scene, 0, VK_NULL_HANDLE };
            m_worker_queue.push(payload);
            std::erase_if(m_prepare_scenes, [purge_scene](const QData& stored) { return stored.scene == purge_scene; });
            m_purge_queue.pop();
        }
    }
}

void SceneHost::worker_loop()
{
    while (true) {
        QData job;
        m_worker_queue.pop(job);
        if (job.scene == nullptr && job.ticket == 0)
            return;

        if (job.commands == VK_NULL_HANDLE) {
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p teardown", job.scene);
            delete job.scene;
        } else {
            VkCommandBufferBeginInfo begin_info {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_DEMAND(vkBeginCommandBuffer(job.commands, &begin_info));
            job.scene->construct(r_renderer, job.commands);
            vkEndCommandBuffer(job.commands);
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup=%p", job.scene, job.ticket, job.commands);
            m_render_queue.push(job);
        }
    }
}

uint64_t SceneHost::add(IScene* scene)
{
    VkCommandBuffer prepare_commands = m_spare_commands.top();
    uint64_t ticket = m_max_ticket++;
    QData payload = { scene, ticket, prepare_commands };
    if (m_worker_queue.try_push(payload)) {
        m_spare_commands.pop();
        m_prepare_scenes.push_back(std::move(payload));
        return ticket;
    } else {
        return 0;
    }
}

void SceneHost::set_next_scene(uint64_t ticket)
{
    if (ticket == 0)
        m_requested_scene = nullptr;
    else {
        auto it = std::find_if(m_prepare_scenes.begin(), m_prepare_scenes.end(), [ticket](const QData& it) {
            return it.ticket == ticket;
        });
        if (it != m_prepare_scenes.end())
            m_requested_scene = &*it;
    }
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
    std::vector<VkCommandBuffer> all_commands;
    uint64_t max_ticket = 0;
    while (m_render_queue.try_pop(job)) {
        all_commands.push_back(job.commands);
        max_ticket = std::max(max_ticket, job.ticket);
    }

    if (all_commands.size() > 0) {
        VkSubmitInfo submit {};
        VkTimelineSemaphoreSubmitInfo timeline_info {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline_info;
        submit.commandBufferCount = all_commands.size();
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
