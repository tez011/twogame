#pragma once
#include <span>
#include <volk.h>
#include "assets/asset.h"
#include "render/gpu_structs.h"

namespace twogame::asset {

class Mesh final : public IAsset {
    size_t m_vertex_count, m_index_count;
    uint32_t m_displacement_count, m_joint_count, m_material_index;

    union {
        uint32_t m_pipeline_key;
        struct {
            unsigned m_uv_channels : 4;
            unsigned m_color_channels : 2;
            unsigned m_32bit_indexes : 1;
            unsigned m_alpha_mask : 1;
            unsigned m_double_sided : 1;
            unsigned m_unlit : 1;
            unsigned m_transmission : 1;
            unsigned m_volume : 1;
            unsigned m_clearcoat : 1;
            unsigned m_sheen : 1;
            unsigned m_iridescent : 1;
            unsigned m_subsurface_scatter : 1;
        };
    };

public:
    Mesh(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Mesh();
    inline virtual Type type() const override { return IAsset::Type::Mesh; }
    inline size_t index_count() const { return m_index_count; }
    inline uint32_t displacement_count() const { return m_displacement_count; }
    inline uint32_t pipeline_key() const { return m_pipeline_key; }
    inline uint32_t material_index() const { return m_material_index; }
    size_t write_buffer_addresses(std::span<MeshEntry> mesh_entries, VkDeviceAddress base) const;

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

}
