#pragma once
#include <cstddef>
#include <volk.h>
#include "assets/asset.h"
#include "vk_mem_alloc.h"

namespace twogame::asset {

class Image final : public IAsset {
    VkImage m_image;
    VmaAllocation m_mem;
    VkImageView m_image_view;

public:
    Image(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Image();
    inline virtual Type type() const override { return IAsset::Type::Image; }
    inline VkImage handle() const { return m_image; }
    inline VkImageView view() const { return m_image_view; }

    virtual size_t prepare_needs() const override;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

}
