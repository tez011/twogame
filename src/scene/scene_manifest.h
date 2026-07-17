#pragma once
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include "assets/asset.h"
#include "core/debug.h"
#include "core/node_types.h"
#include "scene/scene_graph.h"

namespace twogame {

namespace fbs {
    struct Scene;
}

class AssetContainer {
protected:
    std::vector<std::shared_ptr<asset::Animation>> m_animations;
    std::vector<std::shared_ptr<asset::Image>> m_images;
    std::vector<std::shared_ptr<asset::Material>> m_materials;
    std::vector<std::shared_ptr<asset::Mesh>> m_meshes;
    std::vector<std::shared_ptr<asset::Skeleton>> m_skeletons;

public:
    virtual ~AssetContainer() { }
    AssetContainer& operator+=(const AssetContainer& other);

    std::vector<std::shared_ptr<asset::Animation>>& animations() { return m_animations; }
    std::span<const std::shared_ptr<asset::Animation>> animations() const { return m_animations; }
    std::vector<std::shared_ptr<asset::Image>>& images() { return m_images; }
    std::span<const std::shared_ptr<asset::Image>> images() const { return m_images; }
    std::vector<std::shared_ptr<asset::Material>>& materials() { return m_materials; }
    std::span<const std::shared_ptr<asset::Material>> materials() const { return m_materials; }
    std::vector<std::shared_ptr<asset::Mesh>>& meshes() { return m_meshes; }
    std::span<const std::shared_ptr<asset::Mesh>> meshes() const { return m_meshes; }
    std::vector<std::shared_ptr<asset::Skeleton>>& skeletons() { return m_skeletons; }
    std::span<const std::shared_ptr<asset::Skeleton>> skeletons() const { return m_skeletons; }
};

class SceneManifest {
    std::shared_ptr<const fbs::Scene> m_manifest;
    AssetContainer m_container;
    SceneGraph m_scenegraph;
    std::vector<CameraNode> m_cameras;
    std::vector<MeshNode> m_meshes;
    std::function<bool(size_t, std::function<void*(size_t)>)> m_slurp_buffer;

public:
    SceneManifest(const std::string& path);
    inline std::shared_ptr<const fbs::Scene> manifest() const { return m_manifest; }
    inline const AssetContainer& assets() const { return m_container; }
    inline const SceneGraph& scene() const { return m_scenegraph; }
    inline std::span<const CameraNode> cameras() const { return m_cameras; }
    inline std::span<const MeshNode> meshes() const { return m_meshes; }

    template <typename T = std::byte>
    std::vector<T> buffer(size_t i) const
    {
        std::vector<T> out;
        auto resize = [&out](size_t nsz) -> void* {
            out.resize((nsz + sizeof(T) - 1) / sizeof(T));
            return out.data();
        };
        if (m_slurp_buffer(i, resize) == false)
            std::abort();
        return out;
    }
    template <typename T = std::byte>
    std::function<std::vector<T>(size_t)> buffer_resolver() const
    {
        return [slurp = this->m_slurp_buffer](size_t i) -> std::vector<T> {
            std::vector<T> out;
            auto resize = [&out](size_t nsz) -> void* {
                out.resize((nsz + sizeof(T) - 1) / sizeof(T));
                return out.data();
            };
            if (slurp(i, resize) == false)
                std::abort();
            return out;
        };
    }
    using BufferResolver = std::function<std::vector<std::byte>(size_t)>;
};

}
