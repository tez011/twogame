#include "xml.h"
#include <algorithm>
#include <cinttypes>
#include <queue>

namespace twogame::xml::assets {

Animation::Output::Output(const pugi::xml_node& node)
    : m_offset(std::numeric_limits<size_t>::max())
    , m_bone(std::numeric_limits<uint32_t>::max())
    , m_step_interpolate(false)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "target") == 0)
            m_target = it->value();
        else if (strcmp(it->name(), "step") == 0)
            m_step_interpolate = priv::parse_boolean(it->value());
        else if (strcmp(it->name(), "offset") == 0) {
            if (sscanf(it->value(), "%zu", &m_offset) < 1)
                throw Exception(node, "offset");
        } else if (strcmp(it->name(), "bone") == 0) {
            if (sscanf(it->value(), "%" PRIu32, &m_bone) < 1)
                throw Exception(node, "bone");
        }
    }

    if (m_target.empty())
        throw Exception(node, "target");
    if (m_offset == std::numeric_limits<size_t>::max())
        throw Exception(node, "offset");
    if (m_target != "displacements" && m_bone == std::numeric_limits<uint32_t>::max())
        throw Exception(node, "bone");
}

Animation::Animation(const pugi::xml_node& node)
    : m_input_offset(std::numeric_limits<size_t>::max())
    , m_keyframes(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        else if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "keyframes") == 0) {
            if (sscanf(it->value(), "%zu", &m_keyframes) < 1)
                throw Exception(node, "keyframes");
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "input") == 0) {
            for (auto jt = it->attributes_begin(); jt != it->attributes_end(); ++jt) {
                if (strcmp(jt->name(), "offset") == 0) {
                    if (sscanf(jt->value(), "%zu", &m_input_offset) < 1)
                        throw Exception(*it, "offset");
                }
            }
        } else if (strcmp(it->name(), "output") == 0) {
            m_outputs.emplace_back(*it);
        }
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_source.empty())
        throw Exception(node, "source");
    if (m_input_offset == std::numeric_limits<size_t>::max())
        throw Exception(node, "input-offset");
    if (m_keyframes == 0)
        throw Exception(node, "keyframes");
    if (m_outputs.empty())
        throw Exception(node, "outputs");
}

Image::Image(const pugi::xml_node& node)
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

Material::Material(const pugi::xml_node& node, bool asset_context)
    : m_unique(false)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        if (strcmp(it->name(), "shader") == 0)
            m_shader = it->value();
        if (strcmp(it->name(), "unique") == 0) {
            if (asset_context)
                throw Exception(node, "unique");
            if (it->value() && it->value()[0] != 'f' && it->value()[0] != 'F')
                m_unique = true;
        }
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

    if (asset_context) {
        if (m_name.empty())
            throw Exception(node, "name");
        if (m_shader.empty())
            throw Exception(node, "shader");
    } else {
        if (m_props.size() > 0 && m_shader.empty())
            throw Exception(node, "shader");
        if (m_name.empty() && m_shader.empty()) {
            throw Exception(node, "name");
        }
    }
}

Mesh::Attributes::Attribute::Attribute(const pugi::xml_node& node)
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

    if (m_format.empty())
        throw Exception(node, "format");
    if (m_count == 0)
        throw Exception(node, "count");
}

Mesh::Attributes::Attributes(const pugi::xml_node& node)
    : m_interleaved(false)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        } else if (strcmp(it->name(), "interleaved") == 0)
            m_interleaved = priv::parse_boolean(it->value());
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

Mesh::Displacements::Displacement::Displacement(const pugi::xml_node& node)
    : m_weight(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "weight") == 0) {
            if (sscanf(it->value(), "%f", &m_weight) < 1)
                throw Exception(node, "weight");
        }
    }
}

Mesh::Displacements::Displacements(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "displacement") == 0)
            m_displacements.emplace_back(*it);
    }

    if (m_source.empty())
        throw Exception(node, "source");
    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_displacements.size() == 0)
        throw Exception(node, "displacements");
}

Mesh::Indexes::Indexes(const pugi::xml_node& node)
    : m_offset(0)
    , m_count(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "format") == 0)
            m_format = it->value();
        else if (strcmp(it->name(), "topology") == 0)
            m_topology = it->value();
        else if (strcmp(it->name(), "offset") == 0) {
            if (sscanf(it->value(), "%zu", &m_offset) < 1)
                throw Exception(node, "offset");
        } else if (strcmp(it->name(), "count") == 0) {
            if (sscanf(it->value(), "%zu", &m_count) < 1)
                throw Exception(node, "count");
        }
    }

    if (m_source.empty())
        throw Exception(node, "source");
    if (m_format.empty())
        throw Exception(node, "format");
    if (m_count == 0)
        throw Exception(node, "count");
}

Mesh::Mesh(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attributes") == 0)
            m_attributes.emplace_back(*it);
        else if (strcmp(it->name(), "displacements") == 0)
            m_displacements.emplace(*it);
        else if (strcmp(it->name(), "indexes") == 0)
            m_indexes.emplace(*it);
        else if (strcmp(it->name(), "animation") == 0)
            m_animations.emplace_back(*it);
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_attributes.empty())
        throw Exception(node, "attributes");
    if (m_indexes.has_value() == false)
        throw Exception(node, "indexes");
}

Shader::Stage::Specialization::Specialization(const pugi::xml_node& node)
    : m_constant_id(UINT32_MAX)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "id") == 0) {
            if (sscanf(it->value(), "%" PRIu32, &m_constant_id) < 1)
                throw Exception(node, "id");
        }
    }
    m_value = node.text().get();
    if (m_constant_id == UINT32_MAX)
        throw Exception(node, "id");
}

Shader::Stage::Stage(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "stage") == 0)
            m_stage = it->value();
        else if (strcmp(it->name(), "path") == 0)
            m_path = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "specialization") == 0)
            m_specialization.emplace_back(*it);
    }
    if (m_stage.empty())
        throw Exception(node, "stage");
    if (m_path.empty())
        throw Exception(node, "path");
}

Shader::Shader(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "stage") == 0)
            m_stages.emplace_back(*it);
    }
    if (m_name.empty())
        throw Exception(node, "name");
    if (m_stages.size() == 0)
        throw Exception(node, "stage");
}

}

namespace twogame::xml {

Assets::Assets(const pugi::xml_node& node)
{
    bool name_optional = false;
    for (auto it = node.begin(); it != node.end(); ++it) {
        auto type = it->name(), name = it->attribute("name").value();

        if (strcmp(type, "animations") == 0) {
            m_animations.emplace_back(*it);
            name_optional = true;
        } else if (strcmp(type, "image") == 0)
            m_images.emplace_back(*it);
        else if (strcmp(type, "material") == 0)
            m_materials.emplace_back(*it, true);
        else if (strcmp(type, "mesh") == 0)
            m_meshes.emplace_back(*it);
        else if (strcmp(type, "shader") == 0)
            m_shaders.emplace_back(*it);
        else
            throw Exception(node, type);

        if (name[0] == 0 && !name_optional)
            throw Exception(node, "name");
    }
}

}
