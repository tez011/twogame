#include "scene.h"
#include <cinttypes>
#include <numeric>
#include <set>
#include <physfs.h>
#include "asset.fbs.hpp"
#include "asset.h"
#include "display.h"

namespace twogame {

std::unique_ptr<SceneHost> SceneHost::s_self;

void StagingBuffer::advance(size_t offset)
{
    m_tail += offset;
    SDL_assert(m_tail <= size());
}

void StagingBuffer::copy_buffer(VkBuffer dst, std::span<const VkBufferCopy2> regions, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkDeviceSize min_offset = regions.begin()->dstOffset, max_offset = min_offset;
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        min_offset = std::min(it->dstOffset, min_offset);
        max_offset = std::max(it->dstOffset + it->size, max_offset);
    }

    VkBufferMemoryBarrier2& barrier = m_buffer_memory_barriers.emplace_back();
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = DisplayHost::queue_family_index_dma();
    barrier.dstQueueFamilyIndex = DisplayHost::queue_family_index();
    barrier.buffer = dst;
    barrier.offset = min_offset;
    barrier.size = max_offset - min_offset;

    auto& copy = m_buffer_copies.emplace_back();
    copy.second = std::vector(regions.begin(), regions.end());
    copy.first.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copy.first.srcBuffer = m_src_buffer;
    copy.first.dstBuffer = dst;
    copy.first.regionCount = copy.second.size();
    copy.first.pRegions = copy.second.data();
}

void StagingBuffer::copy_image(VkImage dst, const VkImageCreateInfo& info, std::span<const VkBufferImageCopy2> copies, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout final_layout)
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

void StagingBuffer::finalize()
{
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(m_xfer_commands, &begin_info));
    vmaFlushAllocation(DisplayHost::allocator(), m_src_mem, 0, VK_WHOLE_SIZE);

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

    // This is ok to do here, but only because we don't touch the staging buffer between this function
    // and when the command buffers are done executing.
    m_tail = 0;
}

AssetContainer& AssetContainer::operator+=(const AssetContainer& other)
{
    m_images.insert(m_images.end(), other.m_images.begin(), other.m_images.end());
    m_meshes.insert(m_meshes.end(), other.m_meshes.begin(), other.m_meshes.end());
    return *this;
}

SceneGraph::SceneGraph(std::vector<uint32_t>&& parents, std::vector<std::variant<mat4s, TRS>>&& transforms)
{
    m_parents = std::move(parents);
    m_children.assign(m_parents.size(), NONE);
    m_siblings.assign(m_parents.size(), NONE);
    m_local_transforms = std::move(transforms);
    for (size_t i = 0; i < node_count(); i++) {
        uint32_t parent = m_parents[i];
        if (parent != NONE) {
            m_siblings[i] = m_children[parent];
            m_children[parent] = i;
        }
    }
}

uint32_t SceneGraph::insert(uint32_t parent, const std::variant<mat4s, TRS>& xfm)
{
    uint32_t new_index = m_parents.size();
    m_siblings.push_back(m_children[parent]);
    m_children.push_back(NONE);
    m_parents.push_back(parent);
    m_children[parent] = new_index;
    return new_index;
}

uint32_t SceneGraph::insert(uint32_t parent, const SceneGraph& other)
{
    uint32_t other_start = m_parents.size(), new_size = m_parents.size() + other.m_parents.size();
    m_siblings.reserve(new_size);
    m_children.reserve(new_size);
    m_parents.reserve(new_size);

    std::vector<uint32_t> root_siblings;
    for (size_t i = 0; i < other.m_parents.size(); i++) {
        if (other.m_parents[i] == NONE) {
            m_parents.push_back(parent);
            root_siblings.push_back(i + other_start);
        } else {
            m_parents.push_back(other.m_parents[i] + other_start);
        }
    }
    root_siblings.push_back(m_children[parent]);

    auto transform_cs_node_index = [other_start](uint32_t j) { return j == NONE ? NONE : j + other_start; };
    std::transform(other.m_children.begin(), other.m_children.end(), std::back_inserter(m_children), transform_cs_node_index);
    std::transform(other.m_siblings.begin(), other.m_siblings.end(), std::back_inserter(m_siblings), transform_cs_node_index);

    for (int i = 0; i < int(root_siblings.size()) - 1; i++)
        m_siblings[root_siblings[i]] = root_siblings[i + 1];
    m_children[parent] = root_siblings[0];
    return other_start;
}

void SceneGraph::reparent(uint32_t node, uint32_t new_parent)
{
    uint32_t old_parent = m_parents[node];
    if (node == new_parent || old_parent == new_parent)
        return;

    if (old_parent != NONE) {
        // Remove the current node from its parent's child chain
        uint32_t n = m_children[old_parent], prev = NONE;
        while (n != NONE && n != node) {
            prev = n;
            n = m_siblings[n];
        }
        if (n == node) {
            if (prev == NONE) {
                // This node was the first child.
                m_children[old_parent] = m_siblings[node];
            } else {
                m_siblings[prev] = m_siblings[node];
            }
        }
    }

    m_parents[node] = new_parent;
    if (new_parent == NONE) {
        // root nodes have no siblings
        m_siblings[node] = NONE;
    } else {
        m_siblings[node] = m_children[new_parent];
        m_children[new_parent] = node;
    }
}

void SceneGraph::remove(uint32_t node)
{
    if (m_children[m_parents[node]] == node)
        m_children[m_parents[node]] = m_siblings[node];
    for (uint32_t c = m_children[m_parents[node]]; c != NONE; c = m_siblings[c]) {
        if (m_siblings[c] == node)
            m_siblings[c] = m_siblings[node];
    }
}

void SceneGraph::update_global_transforms()
{
    // keep this around to prevent reallocation overhead
    static std::vector<mat4s> local_transforms;
    static std::vector<uint32_t> stack;
    m_global_transforms.resize(m_local_transforms.size());
    local_transforms.resize(std::max(local_transforms.size(), m_local_transforms.size()));
    stack.clear();

    for (size_t i = 0; i < node_count(); i++) {
        local_transforms[i] = std::visit([](auto&& xfm) -> mat4s {
            using T = std::decay_t<decltype(xfm)>;
            if constexpr (std::is_same<T, mat4s>::value)
                return xfm;
            else if constexpr (std::is_same<T, TRS>::value)
                return xfm.transform_matrix();
        },
            m_local_transforms[i]);

        if (m_parents[i] == NONE) {
            m_global_transforms[i] = local_transforms[i];
            stack.push_back(i);
        }
    }
    while (stack.empty() == false) {
        uint32_t i = stack.back();
        stack.pop_back();

        uint32_t parent = m_parents[i], sibling = m_siblings[i], child = m_children[i];
        if (sibling != NONE) {
            if (parent == NONE)
                m_global_transforms[sibling] = local_transforms[sibling];
            else
                m_global_transforms[sibling] = glms_mat4_mul(m_global_transforms[parent], local_transforms[sibling]);
            stack.push_back(sibling);
        }
        if (child != NONE) {
            m_global_transforms[child] = glms_mat4_mul(m_global_transforms[i], local_transforms[child]);
            stack.push_back(child);
        }
    }
}

SceneManifest::SceneManifest(const std::string& path)
{
    PHYSFS_File* fh = PHYSFS_openRead(path.c_str());
    if (fh == nullptr)
        return;

    size_t manifest_size = PHYSFS_fileLength(fh);
    std::shared_ptr<std::byte[]> manifest_data = std::make_shared<std::byte[]>(manifest_size);
    PHYSFS_readBytes(fh, manifest_data.get(), manifest_size);
    PHYSFS_close(fh);
    m_manifest = std::shared_ptr<const fbs::Assets>(manifest_data, fbs::GetAssets(manifest_data.get()));

    m_slurp_buffer = [path_pfx = path.substr(0, path.find_last_of('.'))](size_t i) {
        std::vector<std::byte> out;
        if (i > 0) {
            char path_buf[512];
            snprintf(path_buf, 512, "%s.%u.bin", path_pfx.c_str(), static_cast<unsigned int>(i));
            PHYSFS_File* sfh = PHYSFS_openRead(path_buf);
            out.resize(PHYSFS_fileLength(sfh));
            PHYSFS_readBytes(sfh, out.data(), out.size());
            PHYSFS_close(sfh);
        }
        return out;
    };

    for (size_t i = 0; m_manifest->images() && i < m_manifest->images()->size(); i++)
        m_container.images().emplace_back(std::make_shared<asset::Image>(*this, i, m_container.images().size()));
    for (size_t i = 0; m_manifest->meshes() && i < m_manifest->meshes()->size(); i++)
        m_container.meshes().emplace_back(std::make_shared<asset::Mesh>(*this, i, m_container.meshes().size()));
    for (size_t i = 0; m_manifest->skeletons() && i < m_manifest->skeletons()->size(); i++)
        m_container.skeletons().emplace_back(std::make_shared<asset::Skeleton>(*this, i, m_container.skeletons().size()));

    if (m_manifest->nodes() && m_manifest->roots() && m_manifest->nodes()->size() > 0) {
        std::vector<uint32_t> scene_parents(m_manifest->nodes()->size(), SceneGraph::NONE);
        std::vector<std::variant<mat4s, TRS>> scene_xfm(scene_parents.size(), TRS());
        std::deque<uint32_t> queue(m_manifest->roots()->begin(), m_manifest->roots()->end());
        while (queue.empty() == false) {
            uint32_t node_index = queue.front();
            const fbs::SceneNode* node = m_manifest->nodes()->Get(node_index);
            queue.pop_front();

            for (size_t i = 0; node->children() && i < node->children()->size(); i++) {
                uint32_t child = node->children()->Get(i);
                queue.push_back(child);
                scene_parents[child] = node_index;
            }
            if (node->transform_type() == fbs::Transform::Mat4) {
                mat4s& m = scene_xfm[node_index].emplace<mat4s>();
                memcpy(m.raw, node->transform_as_Mat4(), sizeof(mat4));
            } else if (node->transform_type() == fbs::Transform::TRS) {
                TRS& m = scene_xfm[node_index].emplace<TRS>();
                memcpy(m.translation.raw, node->transform_as_TRS()->translation().v(), sizeof(vec3));
                memcpy(m.rotation.raw, node->transform_as_TRS()->rotation().v(), sizeof(versor));
                memcpy(m.scale.raw, node->transform_as_TRS()->scale().v(), sizeof(vec3));
            }

            if (node->camera() > 0) {
                CameraNode& camera = m_cameras.emplace_back();
                camera.node_index = node_index;
                camera.camera_index = node->camera();
            }
            if (node->mesh()) {
                std::shared_ptr<uint32_t[]> mesh_skin;
                std::shared_ptr<float[]> mesh_weights;
                if (node->skeleton()) {
                    uint32_t node_offset = scene_parents.size();
                    auto skeleton = m_container.skeletons()[node->skeleton().value()];
                    if (node->skin()) {
                        mesh_skin = std::make_shared<uint32_t[]>(node->skin()->size());
                        std::copy(node->skin()->begin(), node->skin()->end(), mesh_skin.get());
                    } else {
                        mesh_skin = std::make_shared<uint32_t[]>(skeleton->joints().size());
                        for (auto it = skeleton->bone_parents().begin(); it != skeleton->bone_parents().end(); ++it) {
                            if (*it == std::numeric_limits<uint32_t>::max()) // the parent of the root of the skeleton
                                scene_parents.push_back(node_index); //         is the mesh node
                            else
                                scene_parents.push_back(*it + node_offset);
                        }
                        for (size_t i = 0; i < skeleton->joints().size(); i++) {
                            mesh_skin[i] = skeleton->joints()[i] + node_offset;
                        }
                        std::copy(skeleton->bone_transforms().begin(), skeleton->bone_transforms().end(), std::back_inserter(scene_xfm));
                    }
                }
                if (node->displace_weights()) {
                    mesh_weights = std::make_shared<float[]>(node->displace_weights()->size());
                    std::copy(node->displace_weights()->begin(), node->displace_weights()->end(), mesh_weights.get());
                }
                for (size_t i = 0; i < node->mesh()->size(); i++) {
                    MeshNode& mesh = m_meshes.emplace_back();
                    mesh.node_index = node_index;
                    mesh.mesh_index = node->mesh()->Get(i);
                    mesh.material_index = 0;
                    mesh.skeleton_index = node->skeleton().value_or(std::numeric_limits<uint32_t>::max());
                    mesh.skin = mesh_skin;
                    mesh.weights = mesh_weights;
                }
            }
        }

        m_scenegraph = SceneGraph(std::move(scene_parents), std::move(scene_xfm));
    }
}

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
    vmaDestroyBuffer(DisplayHost::allocator(), m_binding_zero_buffer.handle, m_binding_zero_buffer.mem);
    for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vmaDestroyBuffer(DisplayHost::allocator(), m_instances_buffer[i].handle, m_instances_buffer[i].mem);
        vmaDestroyBuffer(DisplayHost::allocator(), m_indirect_buffer[i].handle, m_indirect_buffer[i].mem);
    }

    vkDestroyDescriptorPool(DisplayHost::device(), m_descriptor_pool, nullptr);
    for (auto it = m_draw_cmd_pool.begin(); it != m_draw_cmd_pool.end(); ++it)
        vkDestroyCommandPool(DisplayHost::device(), *it, nullptr);
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
        base_vertices_addr += std::static_pointer_cast<asset::Mesh>(m_assets.meshes()[i])->write_buffer_addresses(m_mesh_refs, base_vertices_addr);
    }
    if (m_mesh_refs_buffer.memflags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vmaUnmapMemory(DisplayHost::allocator(), m_mesh_refs_buffer.mem);
        vmaFlushAllocation(DisplayHost::allocator(), m_mesh_refs_buffer.mem, 0, VK_WHOLE_SIZE);
    }

    buffer_ci.size = sizeof(BindingZero) * FRAMES_IN_FLIGHT;
    buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.requiredFlags = 0;
    alloc_ci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &buffer_ci, &alloc_ci, &m_binding_zero_buffer.handle, &m_binding_zero_buffer.mem, &alloc_info));
    m_binding_zero = std::span(static_cast<BindingZero*>(alloc_info.pMappedData), FRAMES_IN_FLIGHT);

    size_t max_instance_count = 1;
    buffer_ci.size = sizeof(InstanceEntry) * max_instance_count;
    buffer_ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    alloc_ci.preferredFlags = 0;
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

    VkDescriptorBufferInfo mesh_refs_write {};
    mesh_refs_write.buffer = m_mesh_refs_buffer.handle;
    mesh_refs_write.offset = 0;
    mesh_refs_write.range = sizeof(MeshEntry) * m_assets.meshes().size();

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

    std::array<VkWriteDescriptorSet, 8> writes {};
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
    writes[4].descriptorCount = sampler_writes.size();
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[4].pImageInfo = sampler_writes.data();
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptor_set[1][0];
    writes[5].dstBinding = 2;
    writes[5].descriptorCount = sampler_writes.size();
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[5].pImageInfo = sampler_writes.data();
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptor_set[0][0];
    writes[6].dstBinding = 3;
    writes[6].descriptorCount = picturebook_writes.size();
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[6].pImageInfo = picturebook_writes.data();
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptor_set[1][0];
    writes[7].dstBinding = 3;
    writes[7].descriptorCount = picturebook_writes.size();
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[7].pImageInfo = picturebook_writes.data();
    vkUpdateDescriptorSets(DisplayHost::device(), writes.size(), writes.data(), 0, nullptr);
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
    vkCmdDrawIndirect(cmd, m_indirect_buffer[frame_number % FRAMES_IN_FLIGHT].handle, 0, 1 /*draw count*/, sizeof(VkDrawIndirectCommand));
    vkEndCommandBuffer(cmd);
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
    staging_createinfo.size = Constants::STAGING_BUFFER_SIZE;
    staging_createinfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_createinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    staging_allocinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    staging_allocinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    for (size_t i = 0; i < BUILDER_THREAD_COUNT; i++) {
        VmaAllocationInfo staging_meminfo;
        VK_DEMAND(vmaCreateBuffer(DisplayHost::allocator(), &staging_createinfo, &staging_allocinfo,
            &m_staging_buffers[i].m_src_buffer, &m_staging_buffers[i].m_src_mem, &staging_meminfo));

        m_staging_buffers[i].m_src_data = std::span(static_cast<std::byte*>(staging_meminfo.pMappedData), Constants::STAGING_BUFFER_SIZE);
        m_staging_buffers[i].m_xfer_commands = builder_commands[0][i];
        m_staging_buffers[i].m_acquire_commands = builder_commands[1][i];
        m_staging_buffers[i].m_post_xfer = builder_sem[i];
    }

    // Prepare the initial scene in-line.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    uint64_t pass = 0;
    auto buckets = initial->begin_construct_assets(m_renderer.get(), m_staging_buffers[0]);
    while (pass < buckets.size()) {
        for (auto it = buckets[pass].begin(); it != buckets[pass].end(); ++it) {
            m_staging_buffers[0].advance((*it)->prepare(initial, m_staging_buffers[0]));
            (*it)->post_prepare(pass + 1);
        }

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
    }
    m_scenes[initial] = pass;
    m_requested_scene = initial;
    m_max_ticket.store(pass + 1, std::memory_order_relaxed);
    initial->end_construct_assets(m_renderer.get());
    initial->tick(0, 0, this);
    initial->render(m_renderer.get(), 0);
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
            scene->render(m_renderer.get(), frame_number);
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
            job.scene = build_job.scene;
            job.commands = &m_staging_buffers[thread_id];
            auto buckets = job.scene->begin_construct_assets(m_renderer.get(), m_staging_buffers[thread_id]);
            for (size_t pass = 0; pass < buckets.size(); pass++) {
                job.ticket = m_max_ticket.fetch_add(1, std::memory_order_relaxed);
                for (auto it = buckets[pass].begin(); it != buckets[pass].end(); ++it) {
                    m_staging_buffers[thread_id].advance((*it)->prepare(job.scene, m_staging_buffers[thread_id]));
                    (*it)->post_prepare(job.ticket);
                }

                m_staging_buffers[thread_id].finalize();
                m_render_queue.push(job);
                SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup=%p", job.scene, job.ticket, job.commands);

                // Because the builder thread blocks until the command buffer we just submitted is complete, we don't need any GPU waiting.
                wait_info.pValues = &job.ticket;
                VK_DEMAND(vkWaitSemaphores(DisplayHost::device(), &wait_info, UINT64_MAX));
            }
            SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "worker thread: scene=%p ticket=%" PRIu64 " bringup complete", job.scene, job.ticket);
            job.scene->end_construct_assets(m_renderer.get());
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
        VkCommandBuffer commands = active_scene->draw_commands(frame_number, subpass);
        vkCmdExecuteCommands(container, 1, &commands);
    }
}

}
