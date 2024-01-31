#include "scene.h"
#include <list>
#include <ranges>
#include <physfs.h>
#include "render.h"
#include "twogame.h"
#include "xml.h"

using namespace std::string_view_literals;

template <class>
inline constexpr bool variant_false_v = false;

namespace twogame::e_components {

static void free_perobject_descriptors(Renderer* renderer, entt::registry& registry, entt::entity e)
{
    auto& g = registry.get<geometry>(e);
    renderer->free_perobject_descriptors(g.m_descriptors, g.m_descriptor_buffers);
}

static void push_dirty_transform(entt::registry& r, entt::entity e)
{
    r.emplace_or_replace<dirty_transform>(e);
}

}

namespace twogame {

Scene::Scene(Twogame* tg, std::string_view path)
    : m_twogame(tg)
{
    xml::Document<xml::Scene> scenedoc(path);
    if (scenedoc.ok() == false) {
        spdlog::critical("failed to parse scene at {}", path);
        std::terminate();
    }

    for (const auto& asset_path : scenedoc->assets()) {
        PHYSFS_Stat stat;
        if (PHYSFS_stat(std::string { asset_path }.c_str(), &stat) == 0) {
            spdlog::error("failed to stat {}", asset_path);
            continue;
        }

        if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
            std::string prefix { asset_path };
            char** rc = PHYSFS_enumerateFiles(asset_path.data());
            prefix += '/';
            for (char** i = rc; *i; i++) {
                std::string fullpath = prefix + *i;
                if (strcmp(fullpath.c_str() + fullpath.length() - 4, ".xml") == 0 && !m_assets.import_assets(fullpath, tg->renderer())) {
                    spdlog::error("failed to import assets at {}", fullpath);
                }
            }
        } else if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
            if (!m_assets.import_assets(asset_path, tg->renderer())) {
                spdlog::error("failed to import assets at {}", asset_path);
            }
        }
    }

    m_registry.on_update<e_components::hierarchy>().connect<&e_components::push_dirty_transform>();
    m_registry.on_update<e_components::orientation>().connect<&e_components::push_dirty_transform>();
    m_registry.on_update<e_components::orientation>().connect<&e_components::push_dirty_transform>();
    m_registry.on_destroy<e_components::geometry>().connect<&e_components::free_perobject_descriptors>(m_twogame->renderer());

    std::vector<entt::entity> scene_entities(scenedoc->entities().size());
    m_registry.create(scene_entities.begin(), scene_entities.end());
    for (size_t i = 0; i < scene_entities.size(); i++) {
        const auto& e = scene_entities[i];
        const auto& einfo = scenedoc->entities().at(i);
        m_registry.emplace<e_components::hierarchy>(e);
        m_registry.emplace<e_components::translation>(e, glm::vec3(0.f, 0.f, 0.f));
        m_registry.emplace<e_components::orientation>(e, glm::quat(1.f, 0.f, 0.f, 0.f));
        m_registry.emplace<e_components::transform>(e);
        if (einfo.name().empty() == false)
            m_named_entities[entt::hashed_string(einfo.name().data(), einfo.name().size()).value()] = scene_entities[i];

        for (auto it = einfo.components().begin(); it != einfo.components().end(); ++it) {
            std::visit([&](auto&& ecomp) {
                using C = std::decay_t<decltype(ecomp)>;
                using E = xml::Scene::Entity;
                if constexpr (std::is_same_v<C, E::Camera>) {
                    m_registry.emplace<e_components::camera>(e);
                } else if constexpr (std::is_same_v<C, E::Geometry>) {
                    auto mesh_it = m_assets.meshes().find(ecomp.mesh());
                    if (mesh_it == m_assets.meshes().end()) {
                        spdlog::error("unknown mesh '{}'", ecomp.mesh());
                        return;
                    }

                    std::shared_ptr<asset::Material> material;
                    if (ecomp.material()->name().empty()) {
                        auto shader_it = m_assets.shaders().find(ecomp.material()->shader());
                        if (shader_it == m_assets.shaders().end()) {
                            spdlog::error("unknown shader '{}'", ecomp.material()->shader());
                            return;
                        }

                        material = std::make_shared<asset::Material>(ecomp.material().value(), m_assets);
                    } else {
                        auto material_it = m_assets.materials().find(ecomp.material()->name());
                        if (material_it == m_assets.materials().end()) {
                            spdlog::error("unknown material '{}'", ecomp.material()->name());
                            return;
                        }
                        if (ecomp.material()->unique())
                            material = std::make_shared<asset::Material>(*material_it->second.get());
                        else
                            material = material_it->second;
                    }

                    auto& geometry = m_registry.emplace<e_components::geometry>(e, mesh_it->second, material);
                    (void)material->shader()->graphics_pipeline(mesh_it->second.get());
                    m_twogame->renderer()->create_perobject_descriptors(geometry.m_descriptors, geometry.m_descriptor_buffers); // Here we allocate buffers for per-object data
                    write_perobject_descriptors(e, geometry); // Here we write buffers and images for per-object-prototype data
                } else if constexpr (std::is_same_v<C, E::Rigidbody>) {
                    m_registry.replace<e_components::translation>(e, ecomp.translation());
                    m_registry.replace<e_components::orientation>(e, glm::normalize(ecomp.orientation()));
                } else if constexpr (!std::is_same_v<C, std::monostate>) {
                    static_assert(variant_false_v<C>, "entity xml parser: non-exhaustive visitor");
                }
            },
                *it);
        }
    }

    // Add entities to hierarchy.
    for (size_t i = 0; i < scene_entities.size(); i++) {
        const auto& e = scene_entities[i];
        const auto& parent_name = scenedoc->entities().at(i).parent();
        if (parent_name.empty())
            continue;

        auto parent_it = m_named_entities.find(entt::hashed_string(parent_name.data(), parent_name.size()).value());
        if (parent_it == m_named_entities.end()) {
            spdlog::error("unknown entity '{}'", parent_name);
            continue;
        }

        entt::entity sibling = m_registry.get<e_components::hierarchy>(parent_it->second).m_child;
        m_registry.replace<e_components::hierarchy>(scene_entities[i], parent_it->second, entt::null, entt::null, sibling);
        m_registry.patch<e_components::hierarchy>(parent_it->second, [e](auto& h) { h.m_child = e; });
        if (sibling != entt::null)
            m_registry.patch<e_components::hierarchy>(sibling, [e](auto& h) { h.m_prev = e; });
    }
}

void Scene::draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, uint64_t frame_number, const std::array<VkDescriptorSet, 2>& in_descriptor_sets)
{
    auto view = m_registry.view<e_components::geometry, e_components::transform>();
    std::array<VkDescriptorSet, 4> descriptor_sets;
    std::copy(in_descriptor_sets.begin(), in_descriptor_sets.end(), descriptor_sets.begin());

    for (entt::entity e : view) {
        auto& g = view.get<e_components::geometry>(e);
        glm::mat4& m = view.get<e_components::transform>(e);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->graphics_pipeline(g.m_mesh.get()));

        descriptor_sets[2] = g.m_descriptors[frame_number % 2];
        descriptor_sets[3] = g.m_material->descriptor();
        vkCmdPushConstants(cmd, g.m_material->shader()->pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &m);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->pipeline_layout(),
            0, descriptor_sets.size(), descriptor_sets.data(),
            0, nullptr);

        g.m_mesh->bind_buffers(cmd);
        vkCmdDrawIndexed(cmd, g.m_mesh->index_count(), 1, 0, 0, 0);
    }
}

void Scene::update_transforms()
{
    m_registry.sort<e_components::dirty_transform>([this](entt::entity lhs, entt::entity rhs) {
        entt::entity lp = lhs, rp = rhs;
        do {
            lp = m_registry.get<e_components::hierarchy>(lp).m_parent;
            rp = m_registry.get<e_components::hierarchy>(rp).m_parent;
            if (lp == entt::null && rp != entt::null)
                return true;
            else if (lp != entt::null && rp == entt::null)
                return false;
        } while (lp != entt::null && rp != entt::null);
        return lhs < rhs;
    });
    for (entt::entity e : m_registry.view<e_components::dirty_transform>()) {
        entt::entity p = m_registry.get<e_components::hierarchy>(e).m_parent;
        glm::mat4 local_xfm = glm::translate(glm::mat4(1.f), m_registry.get<e_components::translation>(e)) * glm::mat4_cast(m_registry.get<e_components::orientation>(e));
        if (p == entt::null)
            m_registry.replace<e_components::transform>(e, local_xfm);
        else
            m_registry.replace<e_components::transform>(e, m_registry.get<e_components::transform>(p) * local_xfm);
    }
    m_registry.clear<e_components::dirty_transform>();

    auto cameras = m_registry.view<e_components::camera>();
    if (cameras.begin() != cameras.end()) {
        m_camera_view = glm::inverse(m_registry.get<e_components::transform>(*cameras.begin()));
    }
}

void Scene::write_perobject_descriptors(entt::entity e, e_components::geometry& g)
{
    std::array<VkWriteDescriptorSet, 1> writes;
    std::array<VkDescriptorImageInfo, 1> wimages;
    writes[0] = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,
        0,
        0,
        1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        &wimages[0],
        nullptr,
        nullptr
    };
    wimages[0] = { VK_NULL_HANDLE, g.m_mesh->position_displacement(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    for (auto it = g.m_descriptors.begin(); it != g.m_descriptors.end(); ++it) {
        for (auto jt = writes.begin(); jt != writes.end(); ++jt)
            jt->dstSet = *it;
        vkUpdateDescriptorSets(m_twogame->renderer()->device(), writes.size(), writes.data(), 0, nullptr);
    }

    memcpy(m_twogame->renderer()->perobject_buffer_pool(0)->buffer_memory(g.m_descriptor_buffers[0]),
        g.m_mesh->displacement_initial_weights().data(),
        g.m_mesh->displacement_initial_weights().size() * sizeof(float));
    memcpy(m_twogame->renderer()->perobject_buffer_pool(0)->buffer_memory(g.m_descriptor_buffers[1]),
        g.m_mesh->displacement_initial_weights().data(),
        g.m_mesh->displacement_initial_weights().size() * sizeof(float));
    m_twogame->renderer()->perobject_buffer_pool(0)->flush(g.m_descriptor_buffers.data() + 0, 2);
}

}
