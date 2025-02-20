#pragma once
#include <memory>
#include <sstream>
#include <physfs.h>
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
    Exception(const pugi::xml_node& node, std::string_view prop)
    {
        std::ostringstream oss;
        oss << node.path() << ": bad '" << prop << "'";
        m_what = oss.str();
    }

    virtual const char* what() const noexcept { return m_what.c_str(); }
};

#ifdef XML_DEFINE_DOCUMENT
template <typename T>
class Document {
private:
    pugi::xml_document m_doc;
    std::unique_ptr<T> m_xml;

    bool slurp(const std::string& path)
    {
        PHYSFS_Stat stat;
        PHYSFS_File* fh;

        if (PHYSFS_stat(path.c_str(), &stat) == 0)
            return false;
        if ((fh = PHYSFS_openRead(path.c_str())) == nullptr)
            return false;

        void* buffer = pugi::get_memory_allocation_function()(stat.filesize);
        if (PHYSFS_readBytes(fh, buffer, stat.filesize) < stat.filesize) {
            pugi::get_memory_deallocation_function()(buffer);
            PHYSFS_close(fh);
            return false;
        }

        return m_doc.load_buffer_inplace_own(buffer, stat.filesize);
    }

public:
    Document(std::string_view path)
    {
        std::string npath { path };
        if (slurp(npath) && strcmp(m_doc.first_child().name(), T::root_name()) == 0) {
            try {
                m_xml = std::make_unique<T>(m_doc.first_child(), npath);
            } catch (xml::Exception& e) {
                spdlog::error("failed to parse {} as {}: {}", path, T::root_name(), e.what());
            }
        }
    }
    Document(const Document&) = delete;
    Document(Document&& other)
    {
        m_doc = std::move(other.m_doc);
        m_xml = std::move(other.m_xml);
    }

    inline bool ok() const { return m_xml != nullptr; }
    const T* operator->() const { return m_xml.get(); }
};
#endif

}
