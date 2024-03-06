#pragma once
#include <optional>
#include <string_view>
#include <variant>
#include <vector>
#include <cglm/struct.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

namespace twogame::xml {

#define X(TYPE, NAME) \
private:              \
    TYPE m_##NAME;    \
                      \
public:               \
    const TYPE& NAME() const { return m_##NAME; }

typedef std::pair<size_t, size_t> IntPair;
class Exception : public std::exception {
    std::string m_what;

public:
    Exception(const pugi::xml_node& node, std::string_view prop);
    virtual const char* what() const noexcept { return m_what.c_str(); }
};

namespace assets {
    struct Animation {
        struct Output {
            X(std::string_view, target);
            X(size_t, offset);
            X(uint32_t, bone);
            X(bool, step_interpolate);
            Output(const pugi::xml_node&);
        };
        X(std::string_view, name);
        X(std::string_view, source);
        X(size_t, input_offset);
        X(size_t, keyframes);
        X(std::vector<Output>, outputs);
        Animation(const pugi::xml_node&);
    };
    struct Image {
        X(std::string_view, name);
        X(std::string_view, usage);
        X(std::string_view, source);
        Image(const pugi::xml_node&);
    };
    struct Material {
        using MaterialProps = std::vector<std::pair<std::string, std::string_view>>;
        X(std::string_view, name);
        X(std::string_view, shader);
        X(bool, unique);
        X(MaterialProps, props);
        Material(const pugi::xml_node&, bool asset_context);
    };
    struct Mesh {
        struct Attributes {
            struct Attribute {
                X(std::string_view, name);
                X(std::string_view, format);
                X(size_t, count);
                Attribute(const pugi::xml_node&);
            };
            X(std::string_view, source);
            X(IntPair, range);
            X(std::vector<Attribute>, attributes);
            X(bool, interleaved);
            Attributes(const pugi::xml_node&);
        };
        struct Indexes {
            X(std::string_view, source);
            X(std::string_view, format);
            X(std::string_view, topology);
            X(size_t, offset);
            X(size_t, count);
            Indexes(const pugi::xml_node&);
        };
        struct Displacements {
            struct Displacement {
                X(float, weight);
                Displacement(const pugi::xml_node&);
            };
            X(std::string_view, source);
            X(IntPair, range);
            X(std::vector<Displacement>, displacements);
            Displacements(const pugi::xml_node&);
        };
        struct Skeleton {
            struct Joint {
                X(uint32_t, parent);
                X(vec3s, translation);
                X(versors, orientation);
                Joint(const pugi::xml_node&);
            };
            X(std::string_view, source);
            X(IntPair, range);
            X(std::vector<Joint>, joints);
            Skeleton(const pugi::xml_node&);
        };
        X(std::string_view, name);
        X(std::vector<Attributes>, attributes);
        X(std::optional<Displacements>, displacements);
        X(std::optional<Skeleton>, skeleton);
        X(std::optional<Indexes>, indexes);
        X(std::vector<Animation>, animations);
        Mesh(const pugi::xml_node&);
    };
    struct Shader {
        struct Stage {
            struct Specialization {
                X(uint32_t, constant_id);
                X(std::string_view, value);
                Specialization(const pugi::xml_node&);
            };
            X(std::string_view, stage);
            X(std::string_view, path);
            X(std::vector<Specialization>, specialization);
            Stage(const pugi::xml_node&);
        };
        X(std::string_view, name);
        X(std::vector<Stage>, stages);
        Shader(const pugi::xml_node&);
    };
}

struct Assets {
    X(std::vector<assets::Animation>, animations);
    X(std::vector<assets::Image>, images);
    X(std::vector<assets::Material>, materials);
    X(std::vector<assets::Mesh>, meshes);
    X(std::vector<assets::Shader>, shaders);
    Assets(const pugi::xml_node&);

    static constexpr const char* const root_name() { return "assets"; }
};

struct Scene {
    struct Entity {
        struct Camera {
            Camera(const pugi::xml_node&) { }
        };
        struct Geometry {
            X(std::string_view, mesh);
            X(std::optional<xml::assets::Material>, material);
            Geometry(const pugi::xml_node&);
        };
        struct Rigidbody {
            X(bool, physics);
            X(vec3s, translation);
            X(versors, orientation);
            Rigidbody(const pugi::xml_node&);
        };
        struct BlendShapeAnimation {
            X(std::string_view, initial_animation);
            BlendShapeAnimation(const pugi::xml_node&);
        };
        struct JointAnimation {
            X(std::string_view, initial_animation);
            JointAnimation(const pugi::xml_node&);
        };
        using EntityComponent = std::variant<Camera, Geometry, Rigidbody, BlendShapeAnimation, JointAnimation>;

        X(std::string_view, name);
        X(std::string_view, parent);
        X(std::vector<EntityComponent>, components);
        Entity(const pugi::xml_node&);
    };
    X(std::vector<std::string_view>, assets);
    X(std::vector<Entity>, entities);
    Scene(const pugi::xml_node&);

    static constexpr const char* const root_name() { return "scene"; }
};

namespace priv {
    bool slurp(const std::string& path, pugi::xml_document& doc);
    bool parse_boolean(const std::string_view&);
    size_t split(std::vector<std::string_view>& out, const std::string_view& in, std::string_view delim);
}

#undef X

template <typename T>
class Document {
private:
    pugi::xml_document m_doc;
    T* m_xml;

public:
    Document(std::string_view path)
        : m_xml(nullptr)
    {
        if (xml::priv::slurp(std::string { path }, m_doc) && strcmp(m_doc.first_child().name(), T::root_name()) == 0) {
            try {
                m_xml = new T(m_doc.first_child());
            } catch (xml::Exception& e) {
                spdlog::error("failed to parse {} as {}: {}", path, T::root_name(), e.what());
                m_xml = nullptr;
            }
        }
    }
    Document(const Document&) = delete;
    Document(Document&& other)
    {
        m_doc = std::move(other.m_doc);
        m_xml = other.m_xml;
        other.m_xml = nullptr;
    }
    ~Document()
    {
        delete m_xml;
    }

    inline bool ok() const { return m_xml != nullptr; }
    const T* operator->() const { return m_xml; }
};

}
