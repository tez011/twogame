#pragma once
#include "assets/asset.h"
#include "render/gpu_structs.h"

namespace twogame::asset {

class Material final : public IAsset {
    MaterialEntry m_entry;

public:
    Material(const SceneManifest& source, size_t source_index, size_t dst_index);
    ~Material();
    inline virtual Type type() const override { return IAsset::Type::Material; }
    const MaterialEntry* operator->() const { return &m_entry; }

    void advance_images(size_t image_count);

    virtual size_t prepare_needs() const override { return sizeof(MaterialEntry); }
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) override;
};

}
