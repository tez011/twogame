#include "xml.h"
#include <algorithm>
#include <queue>
#include <sstream>
#include <physfs.h>

namespace twogame::xml {

Exception::Exception(const pugi::xml_node& node, std::string_view prop)
{
    std::ostringstream oss;
    oss << node.path() << ": bad '" << prop << "'";
    m_what = oss.str();
}

Scene::Material::Material(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        if (strcmp(it->name(), "shader") == 0)
            m_shader = it->value();
    }

    std::queue<std::pair<std::string, pugi::xml_node>> Q;
    Q.push({ "", node });

    while (Q.empty() == false) {
        std::string prefix = std::move(Q.front().first);
        pugi::xml_node root = Q.front().second;
        Q.pop();

        for (auto it = root.begin(); it != root.end(); ++it) {
            const char* name = nullptr;
            for (auto jt = it->attributes_begin(); jt != it->attributes_end(); ++jt) {
                if (strcmp(jt->name(), "name") == 0)
                    name = jt->value();
            }
            if (name == nullptr)
                throw Exception(*it, "name");
            if (it->text().get()[0])
                m_props.emplace_back(prefix + name, it->text().get());
            if (std::any_of(it->begin(), it->end(), [](const pugi::xml_node& j) {
                    return j.type() == pugi::node_element;
                })) {
                Q.push({ prefix + name + "/", *it });
            }
        }
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_shader.empty())
        throw Exception(node, "shader");
}

Scene::Entity::Geometry::Geometry(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "mesh") == 0)
            m_mesh = it->value();
        if (strcmp(it->name(), "shader") == 0)
            m_shader = it->value();
        if (strcmp(it->name(), "material") == 0)
            m_material = it->value();
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
        if (strcmp(it->name(), "material") == 0)
            m_materials.emplace_back(*it);
        else if (strcmp(it->name(), "entity") == 0)
            m_entities.emplace_back(*it);
    }
}

}

namespace twogame::xml::priv {

bool slurp(const std::string& path, pugi::xml_document& doc)
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

    return doc.load_buffer_inplace_own(buffer, stat.filesize);
}

bool parse_boolean(const std::string_view& s)
{
    return s == "true" || s == "yes";
}

size_t split(std::vector<std::string_view>& out, const std::string_view& in, std::string_view delim)
{
    std::string_view::size_type start = 0, end;
    out.reserve(std::count_if(in.begin(), in.end(), [&](char ch) { return delim.find(ch) != std::string_view::npos; }));
    while ((end = in.find_first_of(delim, start)) != std::string_view::npos) {
        out.push_back(in.substr(start, end - start));
        start = in.find_first_not_of(delim, end + 1);
    }
    if (start != std::string_view::npos)
        out.push_back(in.substr(start));
    return out.size();
}

}
