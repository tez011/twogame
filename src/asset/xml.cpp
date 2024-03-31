#include <algorithm>
#include <cinttypes>
#include <queue>
#include <sstream>
#include "asset.h"

namespace twogame::xml::assets {

Animation::Output::Output(const pugi::xml_node& node)
    : m_bone(std::numeric_limits<uint32_t>::max())
    , m_width(1)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "target") == 0) {
            m_target = it->value();
        } else if (strcmp(it->name(), "bone") == 0) {
            if (sscanf(it->value(), "%" PRIu32, &m_bone) < 1)
                throw Exception(node, "bone");
        } else if (strcmp(it->name(), "width") == 0) {
            if (sscanf(it->value(), "%" PRIu32, &m_width) < 1)
                throw Exception(node, "width");
        }
    }
}

Animation::Animation(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
    , m_keyframes(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        } else if (strcmp(it->name(), "keyframes") == 0) {
            if (sscanf(it->value(), "%zu", &m_keyframes) < 1)
                throw Exception(node, "keyframes");
        } else if (strcmp(it->name(), "method") == 0) {
            m_method = it->value();
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "output") == 0) {
            m_outputs.emplace_back(*it);
        }
    }

    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_keyframes == 0)
        throw Exception(node, "keyframes");
    if (m_outputs.empty())
        throw Exception(node, "outputs");
}

Image::Image(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "usage") == 0)
            m_usage = it->value();
        else if (strcmp(it->name(), "source") == 0)
            m_image_source = it->value();
    }

    if (m_usage.empty())
        throw Exception(node, "usage");
    if (m_image_source.empty())
        throw Exception(node, "source");
}

Material::Prop::Prop(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        else if (strcmp(it->name(), "type") == 0)
            m_type = it->value();
    }
    if (node.text())
        m_value = node.text().get();

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_type.empty())
        throw Exception(node, "type");
    if (m_value.empty())
        throw Exception(node, "value");
}

Material::Material(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "shader") == 0)
            m_shader = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "binding") == 0)
            m_props.emplace_back(*it);
    }

    if (m_shader.empty())
        throw Exception(node, "shader");
    if (m_props.empty())
        throw Exception(node, "bindings");
}

Mesh::Primitives::Attributes::Attribute::Attribute(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        if (strcmp(it->name(), "format") == 0)
            m_format = it->value();
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_format.empty())
        throw Exception(node, "format");
}

Mesh::Primitives::Attributes::Attributes(const pugi::xml_node& node)
    : m_interleaved(false)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        } else if (strcmp(it->name(), "interleaved") == 0) {
            m_interleaved = xml::priv::parse_boolean(it->value());
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attribute") == 0)
            m_attributes.emplace_back(*it);
    }

    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_attributes.empty())
        throw Exception(node, "attributes");
}

Mesh::Primitives::Indexes::Indexes(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "count") == 0) {
            if (sscanf(it->value(), "%zu", &m_count) < 1)
                throw Exception(node, "count");
        } else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        }
    }

    if (m_count == 0)
        throw Exception(node, "count");
    if (m_range.second == 0)
        throw Exception(node, "range");
}

Mesh::Primitives::Displacements::Displacements(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0) {
            m_name = it->value();
        } else if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        }
    }

    if (m_name.empty())
        throw Exception(node, "name");
    if (m_range.second == 0)
        throw Exception(node, "range");
}

Mesh::Primitives::Primitives(const pugi::xml_node& node)
    : m_count(0)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "count") == 0) {
            if (sscanf(it->value(), "%zu", &m_count) < 1)
                throw Exception(node, "count");
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "attributes") == 0)
            m_attributes.emplace_back(*it);
        else if (strcmp(it->name(), "indexes") == 0)
            m_indexes.emplace(*it);
        else if (strcmp(it->name(), "displacements") == 0)
            m_displacements.emplace_back(*it);
    }

    if (m_attributes.empty())
        throw Exception(node, "attributes");
    if (m_count == 0)
        throw Exception(node, "count");
}

Mesh::Mesh(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "topology") == 0)
            m_primitive_topology = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "primitives") == 0)
            m_primitives.emplace_back(*it);
        else if (strcmp(it->name(), "shape-weights") == 0 && it->text()) {
            std::istringstream iss(it->text().get());
            while (iss) {
                float f;
                iss >> f;
                if (iss)
                    m_shape_weights.push_back(f);
            }
        }
    }

    if (m_primitives.empty())
        throw Exception(node, "primitives");
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
        if (strcmp(it->name(), "type") == 0)
            m_stage = it->value();
        else if (strcmp(it->name(), "source") == 0)
            m_source = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "specialization") == 0)
            m_specialization.emplace_back(*it);
    }
    if (m_stage.empty())
        throw Exception(node, "type");
    if (m_source.empty())
        throw Exception(node, "source");
}

Shader::Shader(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "stage") == 0)
            m_stages.emplace_back(*it);
    }

    if (m_stages.size() == 0)
        throw Exception(node, "stage");
}

Skeleton::Joint::Joint(const pugi::xml_node& node)
    : m_parent(0)
    , m_translation({ 0.f, 0.f, 0.f })
    , m_orientation({ 0.f, 0.f, 0.f, 1.f })
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "parent") == 0) {
            if (sscanf(it->value(), "%zu", &m_parent) < 1)
                throw Exception(node, "parent");
        } else if (strcmp(it->name(), "translation") == 0) {
            if (sscanf(it->value(), "%f %f %f", &m_translation.x, &m_translation.y, &m_translation.z) < 3)
                throw Exception(node, "translation");
        } else if (strcmp(it->name(), "orientation") == 0) {
            if (sscanf(it->value(), "%f %f %f %f", &m_orientation.x, &m_orientation.y, &m_orientation.z, &m_orientation.w) < 4)
                throw Exception(node, "orientation");
        }
    }
}

Skeleton::Skeleton(const pugi::xml_node& node, const Assets& root)
    : AssetBase(node, root)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "range") == 0) {
            if (sscanf(it->value(), "%zu %zu", &m_range.first, &m_range.second) < 2)
                throw Exception(node, "range");
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "joint") == 0)
            m_joints.emplace_back(*it);
    }

    if (m_range.second == 0)
        throw Exception(node, "range");
    if (m_joints.size() == 0)
        throw Exception(node, "joints");
}

AssetBase::AssetBase(const pugi::xml_node& node, const Assets& root)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
    }

    m_source = root.source();
}

}

namespace twogame::xml {

Assets::Assets(const pugi::xml_node& node, const std::string& path)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "source") == 0)
            m_source = util::resolve_path(path, it->value());
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "animation") == 0)
            m_animations.emplace_back(*it, *this);
        else if (strcmp(it->name(), "image") == 0)
            m_images.emplace_back(*it, *this);
        else if (strcmp(it->name(), "material") == 0)
            m_materials.emplace_back(*it, *this);
        else if (strcmp(it->name(), "mesh") == 0)
            m_meshes.emplace_back(*it, *this);
        else if (strcmp(it->name(), "shader") == 0)
            m_shaders.emplace_back(*it, *this);
        else if (strcmp(it->name(), "skeleton") == 0)
            m_skeletons.emplace_back(*it, *this);
        else
            throw Exception(node, it->name());
    }

    if (m_source.empty())
        throw Exception(node, "source");
}

}
