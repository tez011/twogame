#pragma once
#include <optional>
#include <string_view>
#include <variant>
#include <vector>
#include <cglm/struct.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

namespace twogame::xml {

#define XML_FIELD(TYPE, NAME) \
private:                      \
    TYPE m_##NAME;            \
                              \
public:                       \
    const TYPE& NAME() const { return m_##NAME; }

typedef std::pair<size_t, size_t> IntPair;
class Exception : public std::exception {
    std::string m_what;

public:
    Exception(const pugi::xml_node& node, std::string_view prop);
    virtual const char* what() const noexcept { return m_what.c_str(); }
};

namespace priv {
    bool slurp(const std::string& path, pugi::xml_document& doc);
    bool parse_boolean(const std::string_view&);
}

template <typename T>
class Document {
private:
    pugi::xml_document m_doc;
    std::string m_path;
    T* m_xml;

public:
    Document(std::string_view path)
        : m_path(std::string { path })
        , m_xml(nullptr)
    {
        if (xml::priv::slurp(m_path, m_doc) && strcmp(m_doc.first_child().name(), T::root_name()) == 0) {
            try {
                m_xml = new T(m_doc.first_child(), m_path);
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
