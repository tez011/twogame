#pragma once
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <optional>
#include <pugixml.hpp>
#include <SDL_log.h>
#include <string_view>
#include <vector>

namespace twogame::xml {

#define X(TYPE, NAME) \
private:              \
    TYPE m_##NAME;    \
                      \
public:               \
    const TYPE& NAME() { return m_##NAME; }

class Exception : public std::exception {
    std::string m_what;

public:
    Exception(const pugi::xml_node& node, std::string_view prop);
    virtual const char* what() const noexcept { return m_what.c_str(); }
};

struct Assets {
    using IntPair = std::pair<size_t, size_t>;
    struct Image {
        X(std::string_view, name);
        X(std::string_view, usage);
        X(std::string_view, source);
        Image(const pugi::xml_node&);
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
            Attributes(const pugi::xml_node&);
        };
        X(std::string_view, name);
        X(std::vector<Attributes>, attributes);
        X(std::optional<Attributes>, indexes);
        Mesh(const pugi::xml_node&);
    };
    X(std::vector<Image>, images);
    X(std::vector<Mesh>, meshes);
    Assets(const pugi::xml_node&);

    static constexpr const char* const root_name() { return "assets"; }
};

struct Scene {
    struct Entity {
        struct Geometry {
            struct Shader {
                X(std::string_view, stage);
                X(std::string_view, path);
                Shader(const pugi::xml_node&);
            };
            struct Material {
                using MaterialProps = std::vector<std::pair<std::string, std::string_view>>;
                X(MaterialProps, props);
                Material(const pugi::xml_node&);
            };
            X(std::string_view, mesh);
            X(std::vector<Shader>, shaders);
            X(std::optional<Material>, material);
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
    X(std::vector<Entity>, entities);
    Scene(const pugi::xml_node&);

    static constexpr const char* const root_name() { return "scene"; }
};

namespace priv {
    bool slurp(const std::string& path, pugi::xml_document& doc);
    bool parse_boolean(std::string_view);
}

}

namespace twogame {

template <typename T>
class XmlDocument {
private:
    pugi::xml_document m_doc;
    T* m_xml;

public:
    XmlDocument(const std::string& path)
        : m_xml(nullptr)
    {
        if (xml::priv::slurp(path, m_doc) && strcmp(m_doc.first_child().name(), T::root_name()) == 0) {
            try {
                m_xml = new T(m_doc.first_child());
            } catch (xml::Exception& e) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to parse %s as %s: %s", path, T::root_name(), e.what());
                m_xml = nullptr;
            }
        }
    }
    XmlDocument(const XmlDocument&) = delete;
    XmlDocument(XmlDocument&& other)
    {
        m_doc = std::move(other.m_doc);
        m_xml = other.m_xml;
        other.m_xml = nullptr;
    }
    ~XmlDocument()
    {
        delete m_xml;
    }

    inline bool ok() const { return m_xml != nullptr; }
    inline operator const T*() const { return m_xml; }
};

}