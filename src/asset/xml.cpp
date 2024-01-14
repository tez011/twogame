#include "xml.h"
#include <cinttypes>

namespace twogame::xml::assets {

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

Mesh::Indexes::Indexes(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
        else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        } else if (strcmp(it->name(), "topology") == 0)
            m_topology = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attribute") == 0)
            m_attribute.emplace(*it);
    }

    if (m_source.empty())
        throw Exception(node, "source");
    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_attribute.has_value() == false)
        throw Exception(node, "attributes");
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
    for (auto it = node.begin(); it != node.end(); ++it) {
        auto type = it->name(), name = it->attribute("name").value();
        if (name[0] == 0)
            throw Exception(node, "name");

        if (strcmp(type, "image") == 0)
            m_images.emplace_back(*it);
        else if (strcmp(type, "mesh") == 0)
            m_meshes.emplace_back(*it);
        else if (strcmp(type, "shader") == 0)
            m_shaders.emplace_back(*it);
        else
            throw Exception(node, type);
    }
}

}
