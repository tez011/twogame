#include "xml.h"
#include <algorithm>
#include <physfs.h>
#include <queue>
#include <sstream>

namespace twogame::xml {

Exception::Exception(const pugi::xml_node& node, std::string_view prop)
{
    std::ostringstream oss;
    oss << node.path() << ": bad '" << prop << "'";
    m_what = oss.str();
}

Assets::Image::Image(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        else if (strcmp(it->name(), "usage") == 0)
            m_usage = it->value();
        else if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_usage.empty())
        throw Exception(node, "usage");
    if (m_source.empty())
        throw Exception(node, "source");
}

Assets::Mesh::Attributes::Attribute::Attribute(const pugi::xml_node& node)
    : m_count(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        else if (strcmp(it->name(), "format") == 0)
            m_format = it->value();
        else if (strcmp(it->name(), "count") == 0) {
            if (sscanf(it->value(), "%zu", &m_count) < 1)
                throw Exception(node, "count");
        }
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_format.empty())
        throw Exception(node, "format");
    if (m_count == 0)
        throw Exception(node, "count");
}

Assets::Mesh::Attributes::Attributes(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "courangent");
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attribute") == 0)
            m_attributes.emplace_back(*it);
    }

    if (m_source.empty())
        throw Exception(node, "source");
    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_attributes.size() == 0)
        throw Exception(node, "attributes");
}

Assets::Mesh::Mesh(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attributes") == 0)
            m_attributes.emplace_back(*it);
        else if (strcmp(it->name(), "indexes") == 0)
            m_indexes.emplace(*it);
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_attributes.empty())
        throw Exception(node, "attributes");
    if (m_indexes.has_value() == false)
        throw Exception(node, "indexes");
}

Assets::Assets(const pugi::xml_node& node)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        auto type = it->name(), name = it->attribute("name").value();
        if (name[0] == 0)
            throw Exception(node, "name");

        if (strcmp(type, "image") == 0)
            m_images.emplace_back(*it);
        else if (strcmp(type, "mesh") == 0)
            m_meshes.emplace_back(*it);
        else
            throw Exception(node, type);
    }
}

Scene::Entity::Geometry::Shader::Shader(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "stage"))
            m_stage = it->value();
    }
    m_path = node.text().get();
    if (m_path.empty())
        throw Exception(node, "path");
}

Scene::Entity::Geometry::Material::Material(const pugi::xml_node& node)
{
    std::queue<std::pair<std::string, pugi::xml_node>> Q;
    Q.push({ "", node });

    while (Q.empty() == false) {
        std::string prefix = std::move(Q.front().first);
        pugi::xml_node root = Q.front().second;
        Q.pop();

        for (auto it = root.begin(); it != root.end(); ++it) {
            std::string key = prefix + it->name();
            std::replace(key.begin(), key.end(), '-', '_');
            if (it->text().get()[0] != 0)
                m_props.emplace_back(key, it->text().get());
            if (std::any_of(it->begin(), it->end(), [](const pugi::xml_node& j) {
                    return j.type() == pugi::node_element;
                })) {
                Q.push({ key + "/", *it });
            }
        }
    }
}

Scene::Entity::Geometry::Geometry(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "mesh") == 0)
            m_mesh = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "shader") == 0)
            m_shaders.emplace_back(*it);
        else if (strcmp(it->name(), "material") == 0)
            m_material.emplace(*it);
    }

    if (m_mesh.empty())
        throw Exception(node, "mesh");
}

Scene::Entity::Rigidbody::Rigidbody(const pugi::xml_node& node)
    : m_translation(0.f)
    , m_orientation(1.f, 0.f, 0.f, 0.f)
{
    bool found_physics = false;
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "physics") == 0) {
            found_physics = true;
            m_physics = priv::parse_boolean(it->value());
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "transform") == 0) {
            for (auto jt = it->attributes_begin(); jt != it->attributes_end(); jt++) {
                if (strcmp(jt->name(), "translation") == 0) {
                    if (sscanf(jt->value(), "%f %f %f", &m_translation.x, &m_translation.y, &m_translation.z) < 3)
                        throw Exception(*it, "translation");
                } else if (strcmp(jt->name(), "orientation") == 0) {
                    if (sscanf(jt->value(), "%f %f %f %f", &m_orientation.x, &m_orientation.y, &m_orientation.z, &m_orientation.w) < 4)
                        throw Exception(*it, "orientation");
                }
            }
        }
    }

    if (!found_physics)
        throw Exception(node, "physics");
}

Scene::Entity::Entity(const pugi::xml_node& node)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "geometry") == 0)
            m_geometry.emplace(*it);
        else if (strcmp(it->name(), "rigidbody") == 0)
            m_rigidbody.emplace(*it);
    }
}

Scene::Scene(const pugi::xml_node& node)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "assets") == 0)
            m_assets.emplace_back(it->attributes_begin()->value());
        else if (strcmp(it->name(), "entity") == 0)
            m_entities.emplace_back(*it);
    }
}

}

namespace twogame::xml::priv {

bool slurp_xml(const std::string& path, pugi::xml_document& doc)
{
    PHYSFS_Stat stat;
    PHYSFS_File* fh;
    if (PHYSFS_stat(path.data(), &stat) == 0)
        return false;
    if ((fh = PHYSFS_openRead(path.data())) == nullptr)
        return false;

    void* buffer = pugi::get_memory_allocation_function()(stat.filesize);
    if (PHYSFS_readBytes(fh, buffer, stat.filesize) < stat.filesize) {
        pugi::get_memory_deallocation_function()(buffer);
        PHYSFS_close(fh);
        return false;
    }

    return doc.load_buffer_inplace_own(buffer, stat.filesize);
}

bool parse_boolean(std::string_view s)
{
    return s == "true" || s == "yes";
}

}