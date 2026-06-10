#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>
#include <volk.h>
#include "display.h"
#include "vk_mem_alloc.h"

namespace twogame {

class IScene;
class StagingBuffer;

namespace asset {
    class Image;
    class Mesh;
}
namespace fbs {
    struct Assets;
}

class AssetManifest {
public:
    using BufferResolver = std::function<std::vector<std::byte>(size_t)>;

private:
    std::shared_ptr<const fbs::Assets> m_manifest;
    BufferResolver m_slurp_buffer;

public:
    AssetManifest(const std::string& path);
    inline std::shared_ptr<const fbs::Assets> manifest() const { return m_manifest; }
    BufferResolver buffer_resolver() const { return m_slurp_buffer; }
    std::vector<std::byte> buffer(size_t i) const { return m_slurp_buffer(i); }
};

class IAsset {
public:
    enum class Type {
        Image,
        Mesh,
    };

protected:
    std::variant<std::shared_ptr<void>, uint64_t> m_prepared;

    IAsset() { }

public:
    virtual ~IAsset() { }
    virtual Type type() const = 0;

    virtual size_t prepare_needs() const = 0;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) = 0;
    void post_prepare(uint64_t ready);
};

class AssetContainer {
    std::vector<std::shared_ptr<asset::Image>> m_images;
    std::vector<std::shared_ptr<asset::Mesh>> m_meshes;

public:
    AssetContainer& operator+=(const AssetManifest& other);
    std::span<const std::shared_ptr<asset::Image>> images() const { return m_images; }
    std::span<const std::shared_ptr<asset::Mesh>> meshes() const { return m_meshes; }
};

}

namespace twogame::asset {

class Image final : public IAsset {
    friend class AssetContainer;

    VkImage m_image;
    VmaAllocation m_mem;
    VkImageView m_image_view;

public:
    Image(const AssetManifest& source, size_t source_index, const AssetContainer& dst, size_t dst_index);
    ~Image();
    inline virtual Type type() const override { return IAsset::Type::Image; }
    inline VkImage handle() const { return m_image; }
    inline VkImageView view() const { return m_image_view; }

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

class Mesh final : public IAsset {
    friend class AssetContainer;

    size_t m_vertex_count, m_index_count;
    union {
        uint32_t m_pipeline_key;
        struct {
            unsigned m_uv_channels : 4;
            unsigned m_color_channels : 2;
            unsigned m_32bit_indexes : 1;
        };
    };

public:
    Mesh(const AssetManifest& source, size_t source_index, const AssetContainer& dst, size_t dst_index);
    ~Mesh();
    inline virtual Type type() const override { return IAsset::Type::Mesh; }
    inline size_t index_count() const { return m_index_count; }
    inline uint32_t pipeline_key() const { return m_pipeline_key; }
    size_t write_buffer_addresses(std::span<IRenderer::MeshEntry> mesh_entries, VkDeviceAddress base) const;

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

}
