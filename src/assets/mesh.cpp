#include "mesh.h"
#include "asset.fbs.hpp"
#include "render/display_host.h"
#include "render/staging_buffer.h"
#include "scene/scene.h"
#include "scene/scene_manifest.h"

namespace twogame::asset {

namespace mesh {

    enum SubBufferID {
        SubBuffer_Index,
        SubBuffer_Position,
        SubBuffer_Normal,
        SubBuffer_Joints,
        SubBuffer_DPosition,
        SubBuffer_DNormal,
        SubBuffer_MAX_VALUE,
    };

    struct prep {
        SceneManifest::BufferResolver buffer_resolver;
        std::array<size_t, SubBuffer_MAX_VALUE> buffers, buffer_sizes;
        size_t mesh_index, joint_count;
        MeshEntry entry;

        prep(const SceneManifest& source, size_t source_index, size_t dst_index)
            : buffer_resolver(source.buffer_resolver())
            , mesh_index(dst_index)
            , entry({})
        {
            const fbs::Assets* manifest = source.manifest().get();
            const fbs::Mesh* info = manifest->meshes()->Get(source_index);
            buffers[SubBuffer_Index] = info->indexes();
            buffers[SubBuffer_Position] = info->positions();
            buffers[SubBuffer_Normal] = info->normals();
            buffers[SubBuffer_Joints] = info->joints();
            buffers[SubBuffer_DPosition] = info->position_displacements();
            buffers[SubBuffer_DNormal] = info->normal_displacements();
            std::transform(buffers.begin(), buffers.end(), buffer_sizes.begin(), [manifest](size_t buffer_index) {
                return buffer_index ? manifest->buffers()->Get(buffer_index - 1) : 0;
            });

            joint_count = buffer_sizes[mesh::SubBuffer_Joints] / ((sizeof(vec4) + sizeof(ivec4)) * info->vertex_count());
            entry.joint_count = 4 * joint_count;
            entry.displacement_count = info->displace_weights() ? info->displace_weights()->size() : 0;
        }
        ~prep() { }

        size_t calculate_buffer_addresses(VkDeviceAddress base_vertices_addr)
        {
            size_t delta = 0;
            entry.index_buffer_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_Index] + 15) & ~15;
            entry.vertex_buffer_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_Position] + 15) & ~15;
            entry.normal_buffer_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_Normal] + 15) & ~15;
            entry.joints_buffer_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_Joints] + 15) & ~15;
            entry.vertex_displacement_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_DPosition] + 15) & ~15;
            entry.normal_displacement_address = base_vertices_addr + delta;
            delta += (buffer_sizes[mesh::SubBuffer_DNormal] + 15) & ~15;
            return delta;
        }
    };

}

Mesh::Mesh(const SceneManifest& source, size_t source_index, size_t dst_index)
{
    std::shared_ptr<mesh::prep> prepare_data = std::make_shared<mesh::prep>(source, source_index, dst_index);
    const fbs::Mesh* info = source.manifest()->meshes()->Get(source_index);
    m_vertex_count = info->vertex_count();
    m_index_count = info->index_count();
    m_displacement_count = info->displace_weights() ? info->displace_weights()->size() : 0;
    m_material_index = info->material();

    m_uv_channels = 2 * (prepare_data->buffer_sizes[mesh::SubBuffer_Position] / (m_vertex_count * sizeof(vec4))) - 2;
    m_color_channels = prepare_data->buffer_sizes[mesh::SubBuffer_Normal] / (m_vertex_count * sizeof(vec4)) - 2;
    switch (prepare_data->buffer_sizes[mesh::SubBuffer_Index] / m_index_count) {
    case 2:
        m_32bit_indexes = false;
        break;
    case 4:
        m_32bit_indexes = true;
        break;
    default:
        std::abort();
    }

    const fbs::Material* mtl_info = source.manifest()->materials()->Get(info->material());
    if (mtl_info->alpha_mask())
        m_alpha_mask = true;
    if (mtl_info->double_sided())
        m_double_sided = true;
    if (mtl_info->unlit())
        m_unlit = true;

    m_prepared = prepare_data;
}

Mesh::~Mesh()
{
}

size_t Mesh::prepare_needs() const
{
    auto p_prepare_data = std::get_if<std::shared_ptr<void>>(&m_prepared);
    if (p_prepare_data) {
        mesh::prep* prepare_data = static_cast<mesh::prep*>(p_prepare_data->get());
        size_t needs = 0;
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++)
            needs += (prepare_data->buffer_sizes[i] + 15) & (~15);
        return needs;
    } else {
        return 0;
    }
}

size_t Mesh::prepare(IScene* scene, StagingBuffer& commands)
{
    mesh::prep* prepare_data = static_cast<mesh::prep*>(std::get<std::shared_ptr<void>>(m_prepared).get());
    VkBufferDeviceAddressInfo bda_info {};
    bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    bda_info.buffer = scene->mesh_data_buffer();

    std::span<std::byte> mesh_data = scene->mesh_data_pointer();
    std::array<std::vector<std::byte>, mesh::SubBuffer_MAX_VALUE> sub_buffers;
    VkDeviceAddress base_vertices_addr = vkGetBufferDeviceAddress(DisplayHost::device(), &bda_info), base_offset = prepare_data->entry.index_buffer_address - base_vertices_addr;
    for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++)
        sub_buffers[i] = prepare_data->buffer_resolver(prepare_data->buffers[i]);

    VkDeviceAddress delta = 0;
    if (mesh_data.empty()) {
        std::vector<VkBufferCopy2> copies;
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++) {
            if (sub_buffers[i].empty())
                continue;

            copies.emplace_back().sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
            copies.back().srcOffset = commands.tail_offset() + delta;
            copies.back().dstOffset = base_offset + delta;
            copies.back().size = sub_buffers[i].size();
            memcpy(commands.tail() + delta, sub_buffers[i].data(), sub_buffers[i].size());
            delta += (sub_buffers[i].size() + 15) & ~15;
        }
        commands.copy_buffer(scene->mesh_data_buffer(), copies, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        return delta;
    } else {
        for (size_t i = 0; i < mesh::SubBuffer_MAX_VALUE; i++) {
            memcpy(mesh_data.subspan(base_offset + delta).data(), sub_buffers[i].data(), sub_buffers[i].size());
            delta += (sub_buffers[i].size() + 15) & ~15;
        }
        return 0;
    }
}

size_t Mesh::get_buffer_addresses(std::span<MeshEntry> mesh_entries, VkDeviceAddress base_vertices_addr) const
{
    auto prepare_data = std::static_pointer_cast<mesh::prep>(std::get<std::shared_ptr<void>>(m_prepared));
    size_t delta = prepare_data->calculate_buffer_addresses(base_vertices_addr);
    mesh_entries[prepare_data->mesh_index] = prepare_data->entry;
    return delta;
}

}
