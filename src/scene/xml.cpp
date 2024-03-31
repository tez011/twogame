#include <sstream>
#include <physfs.h>
#include "scene.h"

namespace twogame::xml {

Scene::Entity::Geometry::BoundMaterial::BoundMaterial(const pugi::xml_node& node)
    : m_immutable(true)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "mutable") == 0)
            m_immutable = !priv::parse_boolean(it->value());
    }

    if (node.text())
        m_name = node.text().get();
    else
        throw Exception(node, "material");
}

Scene::Entity::Geometry::Geometry(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "mesh") == 0)
            m_mesh = it->value();
        if (strcmp(it->name(), "skeleton") == 0)
            m_skeleton = it->value();
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "material") == 0)
            m_materials.emplace_back(*it);
    }

    if (m_mesh.empty())
        throw Exception(node, "mesh");
}

Scene::Entity::Rigidbody::Rigidbody(const pugi::xml_node& node)
    : m_translation(GLMS_VEC3_ZERO)
    , m_orientation(GLMS_QUAT_IDENTITY)
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

Scene::Entity::Animator::Animator(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "initial") == 0)
            m_initial_animation = it->value();
    }
}

Scene::Entity::Entity(const pugi::xml_node& node)
{
    for (auto it = node.attributes_begin(); it != node.attributes_end(); ++it) {
        if (strcmp(it->name(), "name") == 0)
            m_name = it->value();
        else if (strcmp(it->name(), "parent") == 0)
            m_parent = it->value();
    }

    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "camera") == 0)
            m_components.emplace_back<Camera>(*it);
        else if (strcmp(it->name(), "geometry") == 0)
            m_components.emplace_back<Geometry>(*it);
        else if (strcmp(it->name(), "rigidbody") == 0)
            m_components.emplace_back<Rigidbody>(*it);
        else if (strcmp(it->name(), "animator") == 0)
            m_components.emplace_back<Animator>(*it);
    }

    auto ecmp = [](const EntityComponent& lhs, const EntityComponent& rhs) {
        return lhs.index() < rhs.index();
    };
    std::stable_sort(m_components.begin(), m_components.end(), ecmp);
}

Scene::Scene(const pugi::xml_node& node, const std::string& path)
{
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (strcmp(it->name(), "assets") == 0)
            m_assets.emplace_back(it->attributes_begin()->value());
        else if (strcmp(it->name(), "entity") == 0)
            m_entities.emplace_back(*it);
    }
}

}
