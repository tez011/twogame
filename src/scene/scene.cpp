#include "scene.h"
#include <algorithm>
#include <numeric>
#include "assets/image.h"
#include "assets/material.h"
#include "assets/mesh.h"
#include "assets/skeleton.h"
#include "core/debug.h"
#include "core/node_types.h"
#include "render/display_host.h"
#include "render/renderer.h"
#include "render/staging_buffer.h"
#include "scene/scene_host.h"

namespace twogame {

IScene::IScene()
{
    VkCommandPoolCreateInfo cmd_pool_ci {};
    cmd_pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cmd_pool_ci.queueFamilyIndex = DisplayHost::queue_family_index();

    VkCommandBufferAllocateInfo cmd_buffer_ci {};
    cmd_buffer_ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_buffer_ci.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    cmd_buffer_ci.commandBufferCount = m_draw_cmd[0].size();
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VK_DEMAND(vkCreateCommandPool(DisplayHost::device(), &cmd_pool_ci, nullptr, &m_draw_cmd_pool[i]));

        cmd_buffer_ci.commandPool = m_draw_cmd_pool[i];
        VK_DEMAND(vkAllocateCommandBuffers(DisplayHost::device(), &cmd_buffer_ci, m_draw_cmd[i].data()));
    }
}

IScene::~IScene()
{
    vmaDestroyBuffer(DisplayHost::allocator(), m_vertices_buffer.handle, m_vertices_buffer.mem);
    vmaDestroyBuffer(DisplayHost::allocator(), m_mesh_refs_buffer.handle, m_mesh_refs_buffer.mem);
    vmaDestroyBuffer(DisplayHost::allocator(), m_materials_buffer.handle, m_materials_buffer.mem);
    vmaDestroyBuffer(DisplayHost::allocator(), m_binding_zero_buffer.handle, m_binding_zero_buffer.mem);
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vmaDestroyBuffer(DisplayHost::allocator(), m_varying_buffer[i].handle, m_varying_buffer[i].mem);
        vmaDestroyBuffer(DisplayHost::allocator(), m_instances_buffer[i].handle, m_instances_buffer[i].mem);
        vmaDestroyBuffer(DisplayHost::allocator(), m_indirect_buffer[i].handle, m_indirect_buffer[i].mem);
    }

    vkDestroyDescriptorPool(DisplayHost::device(), m_descriptor_pool, nullptr);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(DisplayHost::device(), *it, nullptr);
}

void IScene::animate(uint64_t frame_time)
{
    static std::vector<vec4s> animation_data;
    for (auto it = m_animations.begin(); it != m_animations.end(); ++it) {
        const std::shared_ptr<asset::Animation>& anim = m_assets.animations().at(it->animation_index);
        int64_t anim_time = frame_time - it->start_time;
        if (anim_time < 0)
            continue;
        else if (anim_time > anim->duration() && it->loop)
            anim_time %= anim->duration();
        animation_data.resize(anim->keyframe_width());
        anim->interpolate(anim_time / 1000.f, (vec4*)animation_data.data(), std::span(it->keyframe_hints.get(), anim->total_samplers()));

        for (size_t i = 0, off = 0; i < anim->targets().size(); i++) {
            uint32_t object = anim->targets()[i].object;
            if (const auto* ct = std::get_if<std::unique_ptr<uint32_t[]>>(&it->custom_targets)) {
                if ((*ct)[i] != std::numeric_limits<uint32_t>::max())
                    object = (*ct)[i];
            } else if (anim->targets()[i].object_is_bone) {
                object = std::get<std::weak_ptr<uint32_t[]>>(it->custom_targets).lock()[object];
            }
            switch (anim->targets()[i].field) {
            case AnimationTarget::Field::Translation:
                m_scenegraph.transform(object).translation = glms_vec4_copy3(animation_data[off++]);
                break;
            case AnimationTarget::Field::Rotation:
                memcpy(m_scenegraph.transform(object).rotation.raw, animation_data[off++].raw, sizeof(versor));
                break;
            case AnimationTarget::Field::Scale:
                m_scenegraph.transform(object).scale = glms_vec4_copy3(animation_data[off++]);
                break;
            case AnimationTarget::Field::Weights:
                if (m_sparse_meshes[object] != std::numeric_limits<uint32_t>::max())
                    memcpy(m_draw_meshes[m_sparse_meshes[object]].weights.get(), animation_data[off].raw,
                        m_assets.meshes().at(m_draw_meshes[m_sparse_meshes[object]].mesh_index)->displacement_count() * sizeof(float));
                // Since all meshes that share a node have the same weights, we need only update the head.
                break;
            }
        }
    }
}

void IScene::record_commands(IRenderer* renderer, uint32_t frame_number)
{
    auto allocations = std::to_array({
        m_instances_buffer[frame_number % FRAMES_IN_FLIGHT].mem,
        m_indirect_buffer[frame_number % FRAMES_IN_FLIGHT].mem,
        m_binding_zero_buffer.mem,
    });
    std::array<VkDeviceSize, std::size(allocations)> offsets, sizes;
    offsets.fill(0);
    sizes.fill(VK_WHOLE_SIZE);
    vmaFlushAllocations(DisplayHost::allocator(), allocations.size(), allocations.data(), offsets.data(), sizes.data());
    if (m_varying_buffer[frame_number % FRAMES_IN_FLIGHT].mem)
        vmaFlushAllocation(DisplayHost::allocator(), m_varying_buffer[frame_number % FRAMES_IN_FLIGHT].mem, 0, VK_WHOLE_SIZE);
    vkResetCommandPool(DisplayHost::device(), m_draw_cmd_pool[frame_number % FRAMES_IN_FLIGHT], 0);

    VkBufferDeviceAddressInfo bda_info {};
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = m_instances_buffer[frame_number % FRAMES_IN_FLIGHT].handle;
    VkDeviceAddress instance_buffer_address = vkGetBufferDeviceAddress(DisplayHost::device(), &bda_info);

    VkCommandBuffer cmd = m_draw_cmd[frame_number % FRAMES_IN_FLIGHT][0];
    VkCommandBufferBeginInfo begin_info {};
    VkCommandBufferInheritanceInfo inherit_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inherit_info;
    inherit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inherit_info.renderPass = renderer->render_pass();
    inherit_info.subpass = 0;
    VK_DEMAND(vkBeginCommandBuffer(cmd, &begin_info));

    VkExtent2D swapchain_extent = DisplayHost::swapchain_extent();
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

    // bind the pipeline for pass 0 whose pipeline key matches the meshes we're drawing
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->graphics_pipeline(0));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline_layout(0), 0, m_descriptor_set[frame_number % FRAMES_IN_FLIGHT].size(), m_descriptor_set[frame_number % FRAMES_IN_FLIGHT].data(), 0, nullptr);
    vkCmdPushConstants(cmd, renderer->pipeline_layout(0), VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &instance_buffer_address);
    vkCmdDrawIndirect(cmd, m_indirect_buffer[frame_number % FRAMES_IN_FLIGHT].handle, 0, m_draw_meshes.size(), sizeof(VkDrawIndirectCommand));
    vkEndCommandBuffer(cmd);
}

std::vector<std::vector<IAsset*>> IScene::begin_construct_assets(IRenderer* renderer, StagingBuffer& commands)
{
    m_descriptor_pool = renderer->create_descriptor_pool();

    VkDescriptorSetAllocateInfo descriptor_alloc_info {};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_descriptor_alloc_info {};
    descriptor_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_alloc_info.pNext = &variable_descriptor_alloc_info;
    descriptor_alloc_info.descriptorPool = m_descriptor_pool;
    descriptor_alloc_info.descriptorSetCount = renderer->descriptor_set_layouts().size();
    descriptor_alloc_info.pSetLayouts = renderer->descriptor_set_layouts().data();
    variable_descriptor_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variable_descriptor_alloc_info.descriptorSetCount = 1;
    variable_descriptor_alloc_info.pDescriptorCounts = &Constants::PICTUREBOOK_CAPACITY;
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, m_descriptor_set[0].data()));
    VK_DEMAND(vkAllocateDescriptorSets(DisplayHost::device(), &descriptor_alloc_info, m_descriptor_set[1].data()));

    VkBufferCreateInfo buffer_ci {};
    VmaAllocationCreateInfo alloc_ci {};
    VmaAllocationInfo alloc_info;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = 0;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (size_t i = 0; i < m_assets.meshes().size(); i++)
        buffer_ci.size += m_assets.meshes()[i]->prepare_needs();
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_vertices_buffer.handle, &m_vertices_buffer.mem, &alloc_info));
    vmaGetMemoryTypeProperties(DisplayHost::allocator(), alloc_info.memoryType, &m_vertices_buffer.memflags);
    if (m_vertices_buffer.memflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* mapped;
        VK_DEMAND(vmaMapMemory(DisplayHost::allocator(), m_vertices_buffer.mem, &mapped));
        m_vertices_ptr = std::span(static_cast<std::byte*>(mapped), buffer_ci.size);
    } else {
        m_vertices_ptr = std::span(static_cast<std::byte*>(nullptr), 0);
    }

    buffer_ci.size = sizeof(MeshEntry) * m_assets.meshes().size();
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_mesh_refs_buffer.handle, &m_mesh_refs_buffer.mem, &alloc_info));
    vmaGetMemoryTypeProperties(DisplayHost::allocator(), alloc_info.memoryType, &m_mesh_refs_buffer.memflags);
    if (m_mesh_refs_buffer.memflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* mapped;
        VK_DEMAND(vmaMapMemory(DisplayHost::allocator(), m_mesh_refs_buffer.mem, &mapped));
        m_mesh_refs = std::span(static_cast<MeshEntry*>(mapped), m_assets.meshes().size());
    } else {
        m_mesh_refs = std::span(reinterpret_cast<MeshEntry*>(commands.tail()), m_assets.meshes().size());

        VkBufferCopy2 copy {};
        copy.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        copy.srcOffset = commands.tail_offset();
        copy.dstOffset = 0;
        copy.size = buffer_ci.size;
        commands.copy_buffer(m_mesh_refs_buffer.handle, std::span(&copy, 1), VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        commands.advance(copy.size);
    }

    VkBufferDeviceAddressInfo bda_info {};
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = m_vertices_buffer.handle;
    VkDeviceAddress base_vertices_addr = vkGetBufferDeviceAddress(DisplayHost::device(), &bda_info);
    for (size_t i = 0; i < m_assets.meshes().size(); i++) {
        base_vertices_addr += std::static_pointer_cast<asset::Mesh>(m_assets.meshes()[i])->get_buffer_addresses(m_mesh_refs, base_vertices_addr);
    }
    if (m_mesh_refs_buffer.memflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vmaUnmapMemory(DisplayHost::allocator(), m_mesh_refs_buffer.mem);
        vmaFlushAllocation(DisplayHost::allocator(), m_mesh_refs_buffer.mem, 0, VK_WHOLE_SIZE);
    }

    buffer_ci.size = sizeof(MaterialEntry) * std::max(1UL, m_assets.materials().size());
    buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_materials_buffer.handle, &m_materials_buffer.mem, &alloc_info));
    vmaGetMemoryTypeProperties(DisplayHost::allocator(), alloc_info.memoryType, &m_materials_buffer.memflags);
    if (m_materials_buffer.memflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* mapped;
        VK_DEMAND(vmaMapMemory(DisplayHost::allocator(), m_materials_buffer.mem, &mapped));
        m_material_ptr = std::span(static_cast<MaterialEntry*>(mapped), m_assets.materials().size());
    } else {
        m_material_ptr = std::span(static_cast<MaterialEntry*>(nullptr), 0);
    }

    buffer_ci.size = sizeof(BindingZero) * FRAMES_IN_FLIGHT;
    buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.requiredFlags = 0;
    alloc_ci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_binding_zero_buffer.handle, &m_binding_zero_buffer.mem, &alloc_info));
    m_binding_zero = std::span(static_cast<BindingZero*>(alloc_info.pMappedData), FRAMES_IN_FLIGHT);

    size_t max_joints_and_weights = std::transform_reduce(m_draw_meshes.begin(), m_draw_meshes.end(),
        0, std::plus<size_t>(),
        [this](const MeshNode& m) {
            size_t weights = m_assets.meshes()[m.mesh_index]->displacement_count(), // in floats
                joints = m.skeleton_index == std::numeric_limits<uint32_t>::max()
                ? 0
                : m_assets.skeletons()[m.skeleton_index]->joints().size(); // in mat4s
            return joints * 4 + ((weights + 3) >> 2);
        });
    buffer_ci.size = sizeof(vec4) * max_joints_and_weights;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    alloc_ci.preferredFlags = 0;
    if (max_joints_and_weights > 0) {
        for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_varying_buffer[i].handle, &m_varying_buffer[i].mem, &alloc_info));
            m_varying[i] = std::span(static_cast<vec4s*>(alloc_info.pMappedData), max_joints_and_weights);
        }
    }

    size_t max_instance_count = m_draw_meshes.size();
    buffer_ci.size = sizeof(InstanceEntry) * max_instance_count;
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_instances_buffer[i].handle, &m_instances_buffer[i].mem, &alloc_info));
        m_instances[i] = std::span(static_cast<InstanceEntry*>(alloc_info.pMappedData), max_instance_count);
    }
    buffer_ci.size = sizeof(VkDrawIndirectCommand) * max_instance_count;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_indirect_buffer[i].handle, &m_indirect_buffer[i].mem, &alloc_info));
        m_draw_commands[i] = std::span(static_cast<VkDrawIndirectCommand*>(alloc_info.pMappedData), max_instance_count);
    }

    std::vector<IAsset*> all_assets;
    for (auto it = m_assets.meshes().begin(); it != m_assets.meshes().end(); ++it)
        all_assets.push_back(it->get());
    for (auto it = m_assets.materials().begin(); it != m_assets.materials().end(); ++it)
        all_assets.push_back(it->get());
    for (auto it = m_assets.images().begin(); it != m_assets.images().end(); ++it)
        all_assets.push_back(it->get());

    std::vector<std::vector<IAsset*>> buckets(1);
    std::vector<size_t> bucket_usage = { commands.tail_offset() };
    std::sort(all_assets.begin(), all_assets.end(), [](const IAsset* lhs, const IAsset* rhs) {
        return lhs->prepare_needs() > rhs->prepare_needs();
    });
    for (auto it = all_assets.begin(); it != all_assets.end(); ++it) {
        bool unassigned = true;
        size_t occ = (*it)->prepare_needs();
        for (size_t i = 0; i < buckets.size() && unassigned; i++) {
            if (bucket_usage[i] + occ <= commands.size()) {
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
    return buckets;
}

void IScene::end_construct_assets(IRenderer* renderer)
{
    if (m_vertices_ptr.empty() == false)
        vmaUnmapMemory(DisplayHost::allocator(), m_vertices_buffer.mem);

    std::array<VkDescriptorBufferInfo, 2> binding_zero_writes {};
    binding_zero_writes[0].buffer = m_binding_zero_buffer.handle;
    binding_zero_writes[0].offset = 0;
    binding_zero_writes[0].range = sizeof(BindingZero);
    binding_zero_writes[1].buffer = m_binding_zero_buffer.handle;
    binding_zero_writes[1].offset = sizeof(BindingZero);
    binding_zero_writes[1].range = sizeof(BindingZero);

    VkDescriptorBufferInfo mesh_refs_write {}, materials_write {};
    mesh_refs_write.buffer = m_mesh_refs_buffer.handle;
    mesh_refs_write.offset = 0;
    mesh_refs_write.range = sizeof(MeshEntry) * m_assets.meshes().size();
    materials_write.buffer = m_materials_buffer.handle;
    materials_write.offset = 0;
    materials_write.range = sizeof(MaterialEntry) * m_assets.materials().size();

    std::array<VkDescriptorImageInfo, std::tuple_size<decltype(renderer->samplers())>::value> sampler_writes;
    for (size_t i = 0; i < std::tuple_size<decltype(renderer->samplers())>::value; i++) {
        sampler_writes[i].sampler = renderer->samplers().at(i);
        sampler_writes[i].imageView = VK_NULL_HANDLE;
    }
    std::vector<VkDescriptorImageInfo> picturebook_writes(m_assets.images().size());
    for (size_t i = 0; i < m_assets.images().size(); i++) {
        picturebook_writes[i].sampler = VK_NULL_HANDLE;
        picturebook_writes[i].imageView = m_assets.images()[i]->view();
        picturebook_writes[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    std::array<VkWriteDescriptorSet, 10> writes {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptor_set[0][0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &binding_zero_writes[0];
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptor_set[1][0];
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &binding_zero_writes[1];
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptor_set[0][0];
    writes[2].dstBinding = 1;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &mesh_refs_write;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptor_set[1][0];
    writes[3].dstBinding = 1;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &mesh_refs_write;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptor_set[0][0];
    writes[4].dstBinding = 2;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &materials_write;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptor_set[1][0];
    writes[5].dstBinding = 2;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &materials_write;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptor_set[0][0];
    writes[6].dstBinding = 3;
    writes[6].descriptorCount = sampler_writes.size();
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[6].pImageInfo = sampler_writes.data();
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptor_set[1][0];
    writes[7].dstBinding = 3;
    writes[7].descriptorCount = sampler_writes.size();
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[7].pImageInfo = sampler_writes.data();
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_descriptor_set[0][0];
    writes[8].dstBinding = 4;
    writes[8].descriptorCount = picturebook_writes.size();
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[8].pImageInfo = picturebook_writes.data();
    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = m_descriptor_set[1][0];
    writes[9].dstBinding = 4;
    writes[9].descriptorCount = picturebook_writes.size();
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[9].pImageInfo = picturebook_writes.data();
    vkUpdateDescriptorSets(DisplayHost::device(), writes.size(), writes.data(), 0, nullptr);
}

void IScene::handle_base_event(const SDL_Event& evt, SceneHost* stage)
{
    // Handle any events here that are core to all scenes. Otherwise,
    return handle_event(evt, stage);
}

void IScene::tick(uint64_t frame_number, uint64_t frame_time, uint64_t delta_time, SceneHost* stage)
{
    if (m_cameras_dirty) {
        std::sort(m_cameras.begin(), m_cameras.end(), [](const CameraNode& a, const CameraNode& b) {
            return a.camera_index < b.camera_index;
        });
        m_cameras_dirty = false;
    }
    if (m_draw_meshes_dirty) {
        std::sort(m_draw_meshes.begin(), m_draw_meshes.end(), [this](const MeshNode& a, const MeshNode& b) {
            const auto ma = m_assets.meshes()[a.mesh_index], mb = m_assets.meshes()[b.mesh_index];
            if (ma->pipeline_key() != mb->pipeline_key())
                return ma->pipeline_key() < mb->pipeline_key();
            return a.mesh_index < b.mesh_index;
        });
        std::fill(m_sparse_meshes.begin(), m_sparse_meshes.end(), std::numeric_limits<uint32_t>::max());
        for (size_t i = 0; i < m_draw_meshes.size(); i++) {
            m_draw_meshes[i].sibling_dense_mesh = m_sparse_meshes[m_draw_meshes[i].node_index];
            m_sparse_meshes[m_draw_meshes[i].node_index] = i;
        }
        m_draw_meshes_dirty = false;
    }

    animate(frame_time);
    m_scenegraph.update_global_transforms();
    m_binding_zero[frame_number % FRAMES_IN_FLIGHT].proj = stage->renderer()->projection();
    if (m_cameras.size() > 0) {
        m_binding_zero[frame_number % FRAMES_IN_FLIGHT].view = glms_mat4_inv_fast(m_scenegraph.global_transforms()[m_cameras.front().node_index]);
    }
    logic(frame_number, frame_time, delta_time, stage);

    std::span varying = m_varying[frame_number % FRAMES_IN_FLIGHT];
    std::span instances = m_instances[frame_number % FRAMES_IN_FLIGHT];
    std::span draw_commands = m_draw_commands[frame_number % FRAMES_IN_FLIGHT];
    VkDeviceAddress varying_buffer_address = 0;
    if (varying.empty() == false) {
        VkBufferDeviceAddressInfo bda_info {};
        bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bda_info.buffer = m_varying_buffer[frame_number % FRAMES_IN_FLIGHT].handle;
        varying_buffer_address = vkGetBufferDeviceAddress(DisplayHost::device(), &bda_info);
    }

    size_t i = 0, di = 0, v_tail = 0;
    for (i = 0; i < m_draw_meshes.size(); i++) {
        const MeshNode& drawable = m_draw_meshes[i];
        instances[i].model = m_scenegraph.global_transforms()[drawable.node_index];
        instances[i].mesh_id = drawable.mesh_index;
        instances[i].material_id = drawable.material_index;
        if (drawable.skeleton_index != std::numeric_limits<uint32_t>::max()) {
            mat4 inverse_mesh_xfm, relative_joint_xfm;
            const auto& skeleton = m_assets.skeletons()[drawable.skeleton_index];
            std::span<mat4s> joint_matrices = std::span(reinterpret_cast<mat4s*>(varying.subspan(v_tail).data()), skeleton->joints().size());
            instances[i].joint_matrices = varying_buffer_address + v_tail * sizeof(vec4);
            glm_mat4_inv(instances[i].model.raw, inverse_mesh_xfm);
            for (size_t j = 0; j < skeleton->joints().size(); j++) {
                glm_mat4_mul(inverse_mesh_xfm, const_cast<vec4*>(m_scenegraph.global_transforms()[drawable.skin[j]].raw), relative_joint_xfm);
                glm_mat4_mul(relative_joint_xfm, const_cast<vec4*>(skeleton->skin_matrices()[j].raw), joint_matrices[j].raw);
            }
            v_tail += 4 * skeleton->joints().size();
        } else {
            instances[i].joint_matrices = 0;
        }
    }
    for (i = 0; i < m_draw_meshes.size(); i++) {
        if (m_draw_meshes[i].weights) {
            size_t count = m_assets.meshes()[m_draw_meshes[i].mesh_index]->displacement_count();
            instances[i].morph_weights = varying_buffer_address + v_tail * sizeof(vec4);
            memcpy(varying.subspan(v_tail).data(), m_draw_meshes[i].weights.get(), count * sizeof(float));
            v_tail += (count + 3) >> 2;
        } else {
            instances[i].morph_weights = 0;
        }
    }

    size_t last_mesh = 0;
    memset(draw_commands.data(), 0, sizeof(VkDrawIndirectCommand));
    for (i = 0, di = 0; i < m_draw_meshes.size(); i++) {
        if (last_mesh != m_draw_meshes[i].mesh_index) {
            ++di;
            draw_commands[di].instanceCount = 0;
            draw_commands[di].firstVertex = 0;
            draw_commands[di].firstInstance = i;
        }
        draw_commands[di].vertexCount = m_assets.meshes()[m_draw_meshes[i].mesh_index]->index_count();
        draw_commands[di].instanceCount++;
        last_mesh = m_draw_meshes[i].mesh_index;
    }
    memset(draw_commands.subspan(di + 1).data(), 0, sizeof(VkDrawIndirectCommand) * (m_draw_meshes.size() - (di + 1)));
    record_commands(stage->renderer(), frame_number);
}

}
