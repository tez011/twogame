#pragma once
#include <memory>
#include <variant>

namespace twogame {

class IScene;
class SceneManifest;
class StagingBuffer;

class IAsset {
public:
    enum class Type {
        Animation,
        Image,
        Material,
        Mesh,
        Skeleton,
    };

protected:
    std::variant<std::shared_ptr<void>, uint64_t> m_prepared;

    IAsset() { }

public:
    virtual ~IAsset() { }
    virtual Type type() const = 0;

    virtual size_t prepare_needs() const = 0;
    virtual size_t prepare(IScene* scene, StagingBuffer& commands) = 0;
    void post_prepare(uint64_t ready)
    {
        m_prepared = ready;
    }
};

namespace asset {
    class Animation;
    class Image;
    class Material;
    class Mesh;
    class Skeleton;
}

}
