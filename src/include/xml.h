#pragma once
#include <optional>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

namespace twogame::xml {

#define X(TYPE, NAME) \
private:              \
    TYPE m_##NAME;    \
                      \
public:               \
    const TYPE& NAME() const { return m_##NAME; }

class Exception : public std::exception {
    std::string m_what;

public:
    Exception(const pugi::xml_node& node, std::string_view prop);
    virtual const char* what() const noexcept { return m_what.c_str(); }
};

namespace assets {
    struct Image {
        X(std::string_view, name);
        X(std::string_view, usage);
        X(std::string_view, source);
        Image(const pugi::xml_node&);
    };
    struct Mesh {
        using IntPair = std::pair<size_t, size_t>;
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
            X(std::optional<Attributes::Attribute>, attribute);
            X(std::string_view, topology);
            X(std::string_view, source);
            X(IntPair, range);
            Indexes(const pugi::xml_node&);
        };
        X(std::string_view, name);
        X(std::vector<Attributes>, attributes);
        X(std::optional<Indexes>, indexes);
        Mesh(const pugi::xml_node&);
    };
    struct Shader {
        struct RenderSubpass {
            X(uint32_t, render_pass_index);
            X(uint32_t, subpass_index);
            RenderSubpass(const pugi::xml_node&);
        };
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
        X(std::vector<RenderSubpass>, render_passes);
        X(std::vector<Stage>, stages);
        Shader(const pugi::xml_node&);
    };
}

struct Assets {
    X(std::vector<assets::Image>, images);
    X(std::vector<assets::Mesh>, meshes);
    X(std::vector<assets::Shader>, shaders);
    Assets(const pugi::xml_node&);

    static constexpr const char* const root_name() { return "assets"; }
};

struct Scene {
    struct Material {
        using MaterialProps = std::vector<std::pair<std::string, std::string_view>>;
        X(std::string_view, name);
        X(std::string_view, shader);
        X(MaterialProps, props);
        Material(const pugi::xml_node&);
    };
    struct Entity {
        struct Geometry {
            X(std::string_view, mesh);
            X(std::string_view, shader);
            X(std::string_view, material);
            Geometry(const pugi::xml_node&);
        };
        struct Rigidbody {
            X(bool, physics);
            X(glm::vec3, translation);
            X(glm::quat, orientation);
            Rigidbody(const pugi::xml_node&);
        };
        X(std::optional<Geometry>, geometry);
        X(std::optional<Rigidbody>, rigidbody);
        Entity(const pugi::xml_node&);
    };
    X(std::vector<std::string_view>, assets);
    X(std::vector<Material>, materials);
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
