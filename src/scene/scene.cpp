#include "scene.h"
#include <list>
#include <ranges>
#include <physfs.h>
#include "render.h"
#include "twogame.h"
#include "xml.h"

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
    r.emplace_or_replace<transform_dirty>(e);
}

static void push_dirty_morph_weights(entt::registry& r, entt::entity e)
{
    r.emplace_or_replace<morph_weights_dirty_0>(e);
    r.emplace_or_replace<morph_weights_dirty_1>(e);
}

static void push_dirty_joint_mats(entt::registry& r, entt::entity e)
{
    r.emplace_or_replace<joint_mats_dirty_0>(e);
    r.emplace_or_replace<joint_mats_dirty_1>(e);
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
    m_registry.on_update<e_components::translation>().connect<&e_components::push_dirty_transform>();
    m_registry.on_update<e_components::orientation>().connect<&e_components::push_dirty_transform>();
    m_registry.on_update<e_components::morph_weights>().connect<&e_components::push_dirty_morph_weights>();
    m_registry.on_update<e_components::joint_mats>().connect<&e_components::push_dirty_joint_mats>();
    m_registry.on_destroy<e_components::geometry>().connect<&e_components::free_perobject_descriptors>(m_twogame->renderer());

    std::vector<entt::entity> scene_entities(scenedoc->entities().size());
    m_registry.create(scene_entities.begin(), scene_entities.end());
    for (size_t i = 0; i < scene_entities.size(); i++) {
        const auto& e = scene_entities[i];
        const auto& einfo = scenedoc->entities().at(i);
        m_registry.emplace<e_components::hierarchy>(e);
        m_registry.emplace<e_components::translation>(e, 0.f, 0.f, 0.f);
        m_registry.emplace<e_components::orientation>(e, 0.f, 0.f, 0.f, 1.f);
        m_registry.emplace<e_components::transform>(e);
        if (einfo.name().empty() == false)
            m_named_entities[entt::hashed_string(einfo.name().data(), einfo.name().size()).value()] = scene_entities[i];

        // process the geometry first, as other components may depend on mesh information
        auto gcomp = std::find_if(einfo.components().begin(), einfo.components().end(),
            [](const xml::Scene::Entity::EntityComponent& it) {
                return std::holds_alternative<xml::Scene::Entity::Geometry>(it);
            });
        if (gcomp != einfo.components().end()) {
            auto ecomp = std::get<xml::Scene::Entity::Geometry>(*gcomp);
            auto mesh_it = m_assets.meshes().find(ecomp.mesh());
            if (mesh_it == m_assets.meshes().end()) {
                spdlog::error("unknown mesh '{}'", ecomp.mesh());
                return;
            }

            const auto& mesh = mesh_it->second;
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

            if (mesh->default_pose().size() > 0) {
                std::vector<entt::entity> bone_entities(mesh->default_pose().size());
                std::vector<mat4s> bone_mats(mesh->default_pose().size(), GLMS_MAT4_IDENTITY);
                m_registry.create(bone_entities.begin(), bone_entities.end());
                for (size_t j = 0; j < bone_entities.size(); j++) {
                    entt::entity bone_self = bone_entities[j], bone_parent = mesh->default_pose().at(j).parent == 0 ? e : bone_entities[mesh->default_pose().at(j).parent - 1];
                    m_registry.emplace<e_components::bone>(bone_self, e);
                    m_registry.emplace<e_components::hierarchy>(bone_self, bone_parent);
                    m_registry.emplace<e_components::translation>(bone_self, mesh->default_pose().at(j).translation);
                    m_registry.emplace<e_components::orientation>(bone_self, mesh->default_pose().at(j).orientation);
                    m_registry.emplace<e_components::transform>(bone_self);

                    entt::entity bone_sibling = m_registry.get<e_components::hierarchy>(bone_parent).m_child;
                    m_registry.patch<e_components::hierarchy>(bone_self, [bone_sibling](auto& h) { h.m_next = bone_sibling; });
                    m_registry.patch<e_components::hierarchy>(bone_parent, [bone_self](auto& h) { h.m_child = bone_self; });
                    if (bone_sibling != entt::null)
                        m_registry.patch<e_components::hierarchy>(bone_sibling, [bone_self](auto& h) { h.m_prev = bone_self; });
                }

                m_registry.emplace<e_components::joints>(e, std::move(bone_entities));
                m_registry.emplace<e_components::joint_mats>(e, std::move(bone_mats));
            }

            auto& geometry = m_registry.emplace<e_components::geometry>(e, mesh, material);
            (void)material->shader()->graphics_pipeline(mesh.get());
            m_twogame->renderer()->create_perobject_descriptors(geometry.m_descriptors, geometry.m_descriptor_buffers); // Here we allocate buffers for per-object data
            write_perobject_descriptors(e, geometry); // Here we write buffers and images for per-object-prototype data
        }

        for (auto it = einfo.components().begin(); it != einfo.components().end(); ++it) {
            std::visit([&](auto&& ecomp) {
                using C = std::decay_t<decltype(ecomp)>;
                using E = xml::Scene::Entity;
                if constexpr (std::is_same_v<C, E::Camera>) {
                    m_registry.emplace<e_components::camera>(e);
                } else if constexpr (std::is_same_v<C, E::Rigidbody>) {
                    m_registry.replace<e_components::translation>(e, ecomp.translation());
                    m_registry.replace<e_components::orientation>(e, glms_quat_normalize(ecomp.orientation()));
                } else if constexpr (std::is_same_v<C, E::Animator>) {
                    std::shared_ptr<asset::Animation> animation;
                    asset::Mesh* mesh = nullptr;
                    if (m_registry.all_of<e_components::geometry>(e)) {
                        auto& g = m_registry.get<e_components::geometry>(e);
                        auto anim_it = g.m_mesh->animations().find(ecomp.initial_animation());
                        if (anim_it == g.m_mesh->animations().end())
                            anim_it = m_assets.animations().find(ecomp.initial_animation());
                        if (anim_it == m_assets.animations().end()) {
                            spdlog::error("unknown animation '{}'", ecomp.initial_animation());
                            return;
                        }
                        animation = anim_it->second;
                        mesh = g.m_mesh.get();
                    } else {
                        spdlog::warn("skipping application of animation to entity without geometry");
                        return;
                    }

                    switch (animation->type()) {
                    case asset::Animation::AnimationType::BlendShape:
                        m_registry.emplace<e_components::morph_animation>(e, animation, animation, 0ULL, 1.f);
                        m_registry.emplace<e_components::morph_weights>(e, mesh->displacement_initial_weights());
                        m_registry.emplace<e_components::morph_weights_dirty_0>(e);
                        m_registry.emplace<e_components::morph_weights_dirty_1>(e);
                        break;
                    case asset::Animation::AnimationType::Skeleton:
                        m_registry.emplace<e_components::joint_animation>(e, animation, animation, 0ULL, 1.f);
                        m_registry.emplace<e_components::joint_mats_dirty_0>(e);
                        m_registry.emplace<e_components::joint_mats_dirty_1>(e);
                        break;
                    default:
                        assert(false);
                    }
                } else if constexpr (!std::is_same_v<C, E::Geometry> && !std::is_same_v<C, std::monostate>) {
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
        m_registry.patch<e_components::hierarchy>(scene_entities[i], [sibling](auto& h) { h.m_next = sibling; });
        m_registry.patch<e_components::hierarchy>(parent_it->second, [e](auto& h) { h.m_child = e; });
        if (sibling != entt::null)
            m_registry.patch<e_components::hierarchy>(sibling, [e](auto& h) { h.m_prev = e; });
    }
}

void Scene::animate(uint64_t frame_time, uint64_t delta_time)
{
    // first transition animations that are expired, we probably want to tag these with a component eventually
    for (entt::entity e : m_registry.view<e_components::morph_animation>()) {
        const auto& a = m_registry.get<e_components::morph_animation>(e);
        if ((a.m_start_time + static_cast<uint64_t>(1000.f * a.m_animation->duration())) <= frame_time) {
            m_registry.patch<e_components::morph_animation>(e, [](auto& aa) {
                aa.m_start_time += static_cast<uint64_t>(1000.f * aa.m_animation->duration());
                aa.m_animation = aa.m_next_animation;
            });
        }
    }
    for (entt::entity e : m_registry.view<e_components::joint_animation>()) {
        const auto& a = m_registry.get<e_components::joint_animation>(e);
        if ((a.m_start_time + static_cast<uint64_t>(1000.f * a.m_animation->duration())) <= frame_time) {
            m_registry.patch<e_components::joint_animation>(e, [](auto& aa) {
                aa.m_start_time += static_cast<uint64_t>(1000.f * aa.m_animation->duration());
                aa.m_animation = aa.m_next_animation;
            });
        }
    }

    auto morphs = m_registry.view<e_components::geometry, e_components::morph_animation>();
    for (entt::entity e : morphs) {
        const auto& a = morphs.get<e_components::morph_animation>(e);
        if (!a.m_animation)
            continue;

        float t = (frame_time - a.m_start_time) * a.m_multiplier / 1000.f;
        m_registry.patch<e_components::morph_weights>(e, [&](auto& w) {
            a.m_animation->interpolate(t).get(w.m_weights.data(), w.m_weights.size());
        });
    }

    auto skels = m_registry.view<e_components::joint_animation, e_components::joints>();
    for (entt::entity e : skels) {
        const auto& a = skels.get<e_components::joint_animation>(e);
        const auto& b = skels.get<e_components::joints>(e);
        if (!a.m_animation)
            continue;

        float t = (frame_time - a.m_start_time) * a.m_multiplier / 1000.f;
        for (auto it = a.m_animation->interpolate(t); !a.m_animation->finished(it); ++it) {
            if (it.bone() >= b.m_bones.size())
                continue;
            if (it.target() == asset::Animation::ChannelTarget::Translation)
                m_registry.patch<e_components::translation>(b.m_bones[it.bone()], [&it](auto& t) { it.get(t); });
            else if (it.target() == asset::Animation::ChannelTarget::Orientation)
                m_registry.patch<e_components::orientation>(b.m_bones[it.bone()], [&it](auto& o) { it.get(o); });
        }
    }
}

void Scene::update_transforms()
{
    m_registry.sort<e_components::transform_dirty>([this](entt::entity lhs, entt::entity rhs) {
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

    std::set<entt::entity> dirty_skeletons;
    auto dirty_bones = m_registry.view<e_components::transform_dirty, e_components::bone>();
    for (entt::entity e : dirty_bones)
        dirty_skeletons.insert(dirty_bones.get<e_components::bone>(e).m_ancestor);

    auto dirty_transforms = m_registry.view<e_components::transform_dirty, e_components::transform, e_components::hierarchy>();
    for (entt::entity e : dirty_transforms) {
        entt::entity p = dirty_transforms.get<e_components::hierarchy>(e).m_parent;

        mat4s local_xfm = glms_mat4_identity();
        local_xfm = glms_translate(local_xfm, m_registry.get<e_components::translation>(e));
        local_xfm = glms_mat4_mul(local_xfm, glms_quat_mat4(m_registry.get<e_components::orientation>(e)));
        if (p == entt::null)
            m_registry.replace<e_components::transform>(e, local_xfm);
        else
            m_registry.replace<e_components::transform>(e, glms_mat4_mul(dirty_transforms.get<e_components::transform>(p), local_xfm));
    }
    m_registry.clear<e_components::transform_dirty>();

    for (entt::entity e : dirty_skeletons) {
        auto& bones = m_registry.get<e_components::joints>(e).m_bones;
        auto& ibm = m_registry.get<e_components::geometry>(e).m_mesh->inverse_bind_matrices();
        std::vector<mat4s> jm(bones.size());
        for (size_t i = 0; i < bones.size(); i++)
            jm[i] = glms_mat4_mul(m_registry.get<e_components::transform>(bones[i]), const_cast<mat4s&>(ibm[i]));

        m_registry.replace<e_components::joint_mats>(e, std::move(jm));
    }

    auto cameras = m_registry.view<e_components::camera>();
    if (cameras.begin() != cameras.end()) {
        m_camera_view = glms_mat4_inv_fast(m_registry.get<e_components::transform>(*cameras.begin()));
    }
}

void Scene::update_perobject_descriptors()
{
    switch (m_twogame->renderer()->current_frame() % 2) {
    case 0:
        return _update_perobject_descriptors<0>();
    case 1:
        return _update_perobject_descriptors<1>();
    default:
        assert(false);
    }
}

void Scene::draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, uint64_t frame_number, const std::array<VkDescriptorSet, 2>& in_descriptor_sets)
{
    auto view = m_registry.view<e_components::geometry, e_components::transform>();
    std::array<VkDescriptorSet, 4> descriptor_sets;
    std::copy(in_descriptor_sets.begin(), in_descriptor_sets.end(), descriptor_sets.begin());

    for (entt::entity e : view) {
        auto& g = view.get<e_components::geometry>(e);
        mat4s& m = view.get<e_components::transform>(e);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->graphics_pipeline(g.m_mesh.get()));

        descriptor_sets[2] = g.m_descriptors[frame_number % 2];
        descriptor_sets[3] = g.m_material->descriptor();
        vkCmdPushConstants(cmd, g.m_material->shader()->pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4s), &m);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->pipeline_layout(),
            0, descriptor_sets.size(), descriptor_sets.data(),
            0, nullptr);

        g.m_mesh->bind_buffers(cmd);
        vkCmdDrawIndexed(cmd, g.m_mesh->index_count(), 1, 0, 0, 0);
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
}

template <size_t W>
void Scene::_update_perobject_descriptors()
{
    static_assert(W == 0 || W == 1);
    using MWD_t = entt::type_list_element_t<W, entt::type_list<e_components::morph_weights_dirty_0, e_components::morph_weights_dirty_1>>;
    using JMD_t = entt::type_list_element_t<W, entt::type_list<e_components::joint_mats_dirty_0, e_components::joint_mats_dirty_1>>;

    auto morphs = m_registry.view<e_components::morph_weights, e_components::geometry, MWD_t>();
    for (entt::entity e : morphs) {
        auto& g = morphs.template get<e_components::geometry>(e);
        memcpy(m_twogame->renderer()->perobject_buffer_pool(0)->buffer_memory(g.m_descriptor_buffers[0 + W]),
            morphs.template get<e_components::morph_weights>(e).m_weights.data(),
            morphs.template get<e_components::morph_weights>(e).m_weights.size() * sizeof(float));
    }

    auto skels = m_registry.view<e_components::joint_mats, e_components::geometry, JMD_t>();
    for (entt::entity e : skels) {
        auto& g = skels.template get<e_components::geometry>(e);
        memcpy(m_twogame->renderer()->perobject_buffer_pool(1)->buffer_memory(g.m_descriptor_buffers[2 + W]),
            skels.template get<e_components::joint_mats>(e).m_mats.data(),
            skels.template get<e_components::joint_mats>(e).m_mats.size() * sizeof(mat4));
    }

    m_registry.clear<MWD_t>();
    m_registry.clear<JMD_t>();
}

}
