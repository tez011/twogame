#define XML_DEFINE_DOCUMENT
#include "scene.h"
#include <list>
#include <set>
#include <ranges>
#include <physfs.h>
#include "render.h"
#include "twogame.h"
#include "xml/scene.h"

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
    r.emplace_or_replace<transform_dirty_0>(e);
    r.emplace_or_replace<transform_dirty_1>(e);
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
    , m_assets(tg->renderer())
{
    xml::Document<xml::Scene> scenedoc(path);
    if (scenedoc.ok() == false) {
        spdlog::critical("failed to parse scene at {}", path);
        std::terminate();
    }

    for (const auto& raw_asset_path : scenedoc->assets()) {
        std::string asset_path = util::resolve_path(path, raw_asset_path);
        PHYSFS_Stat stat;
        if (PHYSFS_stat(asset_path.c_str(), &stat) == 0) {
            spdlog::error("failed to stat {}", asset_path);
            continue;
        }

        if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
            std::string prefix = asset_path + '/';
            char** rc = PHYSFS_enumerateFiles(asset_path.data());
            for (char** i = rc; *i; i++) {
                std::string fullpath = prefix + *i;
                if (strcmp(fullpath.c_str() + fullpath.length() - 4, ".xml") == 0 && !m_assets.import_assets(fullpath)) {
                    spdlog::error("failed to import assets at {}", fullpath);
                }
            }
            PHYSFS_freeList(rc);
        } else if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
            if (!m_assets.import_assets(asset_path)) {
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

        for (auto it = einfo.components().begin(); it != einfo.components().end(); ++it) {
            std::visit([&](auto&& ecomp) {
                using C = std::decay_t<decltype(ecomp)>;
                using E = xml::Scene::Entity;
                if constexpr (std::is_same_v<C, E::Geometry>) {
                    auto mesh_it = m_assets.meshes().find(ecomp.mesh());
                    if (mesh_it == m_assets.meshes().end())
                        return spdlog::error("unknown mesh '{}'", ecomp.mesh());

                    std::vector<std::shared_ptr<asset::Material>> materials;
                    materials.reserve(ecomp.materials().size());
                    for (auto it = ecomp.materials().begin(); it != ecomp.materials().end(); ++it) {
                        auto mtl_it = m_assets.materials().find(it->name());
                        if (mtl_it == m_assets.materials().end())
                            return spdlog::error("unknown material '{}'", it->name());

                        if (it->immutable())
                            materials.push_back(mtl_it->second);
                        else
                            materials.push_back(mtl_it->second->get_mutable());
                    }

                    std::shared_ptr<asset::Skeleton> skeleton;
                    if (ecomp.skeleton().empty() == false) {
                        auto skel_it = m_assets.skeletons().find(ecomp.skeleton());
                        if (skel_it == m_assets.skeletons().end())
                            return spdlog::error("unknown skeleton '{}'", ecomp.skeleton());
                        skeleton = skel_it->second;

                        std::vector<entt::entity> bone_entities(skeleton->bones());
                        std::vector<mat4s> bone_mats(skeleton->bones(), GLMS_MAT4_IDENTITY);
                        m_registry.create(bone_entities.begin(), bone_entities.end());
                        for (size_t j = 0; j < bone_entities.size(); j++) {
                            auto& bone_info = skeleton->default_pose().at(j);
                            entt::entity bone_self = bone_entities[j], bone_parent = bone_info.parent == 0 ? e : bone_entities[skeleton->default_pose().at(j).parent - 1];
                            m_registry.emplace<e_components::bone>(bone_self, e);
                            m_registry.emplace<e_components::hierarchy>(bone_self, bone_parent);
                            m_registry.emplace<e_components::translation>(bone_self, bone_info.translation);
                            m_registry.emplace<e_components::orientation>(bone_self, bone_info.orientation);
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

                    auto& geometry = m_registry.emplace<e_components::geometry>(e, mesh_it->second, skeleton, materials);
                    m_twogame->renderer()->create_perobject_descriptors(geometry.m_descriptors, geometry.m_descriptor_buffers); // TODO: not all objects need all of these.
                    write_perobject_descriptors(e, geometry);
                } else if constexpr (std::is_same_v<C, E::Camera>) {
                    m_registry.emplace<e_components::camera>(e);
                } else if constexpr (std::is_same_v<C, E::Rigidbody>) {
                    m_registry.replace<e_components::translation>(e, ecomp.translation());
                    m_registry.replace<e_components::orientation>(e, glms_quat_normalize(ecomp.orientation()));
                } else if constexpr (std::is_same_v<C, E::Animator>) {
                    auto anim_it = m_assets.animations().find(ecomp.initial_animation());
                    if (anim_it == m_assets.animations().end())
                        return spdlog::error("unknown animation '{}'", ecomp.initial_animation());

                    const std::shared_ptr<asset::Animation>& animation = anim_it->second;
                    m_registry.emplace<e_components::animation>(e, animation, animation, 0ULL, 1.f);

                    if (animation->is_shapekey()) {
                        auto* g = m_registry.try_get<e_components::geometry>(e);
                        if (!g)
                            return spdlog::error("cannot apply shape-key animation to entity without geometry");

                        m_registry.emplace<e_components::morph_weights>(e, g->m_mesh->morph_weights());
                        m_registry.emplace<e_components::morph_weights_dirty_0>(e);
                        m_registry.emplace<e_components::morph_weights_dirty_1>(e);
                    }

                    if (animation->is_skeleton()) {
                        if (!m_registry.try_get<e_components::joint_mats>(e))
                            return spdlog::error("cannot apply skeletal animation to entity without skeleton");

                        m_registry.emplace<e_components::joint_mats_dirty_0>(e);
                        m_registry.emplace<e_components::joint_mats_dirty_1>(e);
                    }
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
        m_registry.patch<e_components::hierarchy>(scene_entities[i], [sibling](auto& h) { h.m_next = sibling; });
        m_registry.patch<e_components::hierarchy>(parent_it->second, [e](auto& h) { h.m_child = e; });
        if (sibling != entt::null)
            m_registry.patch<e_components::hierarchy>(sibling, [e](auto& h) { h.m_prev = e; });
    }
}

void Scene::animate(uint64_t frame_time, uint64_t delta_time)
{
    auto animateables = m_registry.view<e_components::animation>();
    for (entt::entity e : animateables) {
        const auto& a = animateables.get<e_components::animation>(e);
        const auto* b = m_registry.try_get<e_components::joints>(e);

        // first transition animations that are expired, we probably want to tag these with a component eventually
        if ((a.m_start_time + static_cast<uint64_t>(1000.f * a.m_animation->duration())) <= frame_time) {
            m_registry.patch<e_components::animation>(e, [](auto& aa) {
                aa.m_start_time += static_cast<uint64_t>(1000.f * aa.m_animation->duration());
                aa.m_animation = aa.m_next_animation;
            });
        }
        if (!a.m_animation)
            continue;

        float t = (frame_time - a.m_start_time) * a.m_multiplier / 1000.f;
        for (auto it = a.m_animation->interpolate(t); !it.finished(); ++it) {
            switch (it.target()) {
            case asset::Animation::ChannelTarget::Translation:
                if (b && it.bone() < b->m_bones.size())
                    m_registry.patch<e_components::translation>(b->m_bones[it.bone()], [&it](auto& t) { it.get(t); });
                break;
            case asset::Animation::ChannelTarget::Orientation:
                if (b && it.bone() < b->m_bones.size())
                    m_registry.patch<e_components::orientation>(b->m_bones[it.bone()], [&it](auto& o) { it.get(o); });
                break;
            case asset::Animation::ChannelTarget::ShapeWeights:
                m_registry.patch<e_components::morph_weights>(e, [&it](auto& w) { it.get(w.m_weights.data(), w.m_weights.size()); });
                break;
            default:
                break;
            }
        }
    }
}

void Scene::update_transforms()
{
    std::vector<entt::entity> dirty_transforms(m_registry.view<e_components::transform_dirty>().begin(), m_registry.view<e_components::transform_dirty>().end());
    for (size_t qhead = 0; qhead < dirty_transforms.size(); qhead++) {
        entt::entity child = m_registry.get<e_components::hierarchy>(dirty_transforms[qhead]).m_child;
        while (child != entt::null) {
            dirty_transforms.push_back(child);
            child = m_registry.get<e_components::hierarchy>(child).m_next;
        }
    }
    // dirty_transforms is sorted by depth and contains all dirty transforms and their children.

    std::set<entt::entity> dirty_skeletons;
    auto dirty_bones = m_registry.view<e_components::transform_dirty, e_components::bone>();
    for (entt::entity e : dirty_bones)
        dirty_skeletons.insert(dirty_bones.get<e_components::bone>(e).m_ancestor);

    for (entt::entity e : dirty_transforms) {
        entt::entity p = m_registry.get<e_components::hierarchy>(e).m_parent;

        mat4s local_xfm = glms_mat4_identity();
        local_xfm = glms_translate(local_xfm, m_registry.get<e_components::translation>(e));
        local_xfm = glms_mat4_mul(local_xfm, glms_quat_mat4(m_registry.get<e_components::orientation>(e)));
        if (p == entt::null)
            m_registry.replace<e_components::transform>(e, local_xfm);
        else
            m_registry.replace<e_components::transform>(e, glms_mat4_mul(m_registry.get<e_components::transform>(p), local_xfm));
    }
    m_registry.clear<e_components::transform_dirty>();

    for (entt::entity e : dirty_skeletons) {
        auto& bones = m_registry.get<e_components::joints>(e).m_bones;
        auto& ibm = m_registry.get<e_components::geometry>(e).m_skeleton->inverse_bind_matrices();
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

void Scene::draw(VkCommandBuffer cmd, uint64_t frame_number)
{
    auto view = m_registry.view<e_components::geometry, e_components::transform>();
    for (entt::entity e : view) {
        auto& g = view.get<e_components::geometry>(e);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_twogame->renderer()->pipeline_layout(), 2, 1, &g.m_descriptors[frame_number % 2], 0, nullptr);
        g.m_mesh->draw(cmd, frame_number, g.m_materials);
    }
}

void Scene::write_perobject_descriptors(entt::entity e, e_components::geometry& g)
{
    std::array<VkWriteDescriptorSet, 2> writes;
    std::array<VkDescriptorImageInfo, 2> wimages;
    writes[0] = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,
        static_cast<uint32_t>(Renderer::DescriptorSetSlot::PositionDisplacements),
        0,
        1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        &wimages[0],
        nullptr,
        nullptr
    };
    writes[1] = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,
        static_cast<uint32_t>(Renderer::DescriptorSetSlot::NormalDisplacements),
        0,
        1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        &wimages[1],
        nullptr,
        nullptr
    };
    wimages[0] = {
        VK_NULL_HANDLE,
        g.m_mesh->morph_position() ? g.m_mesh->morph_position() : m_twogame->renderer()->dummy_descriptor<Renderer::DescriptorSetSlot::PositionDisplacements>(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    wimages[1] = {
        VK_NULL_HANDLE,
        g.m_mesh->morph_normal() ? g.m_mesh->morph_normal() : m_twogame->renderer()->dummy_descriptor<Renderer::DescriptorSetSlot::NormalDisplacements>(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

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
    using MMD_t = entt::type_list_element_t<W, entt::type_list<e_components::transform_dirty_0, e_components::transform_dirty_1>>;
    using JMD_t = entt::type_list_element_t<W, entt::type_list<e_components::joint_mats_dirty_0, e_components::joint_mats_dirty_1>>;
    using MWD_t = entt::type_list_element_t<W, entt::type_list<e_components::morph_weights_dirty_0, e_components::morph_weights_dirty_1>>;

    auto modelmats = m_registry.view<e_components::transform, e_components::geometry, MMD_t>();
    for (entt::entity e : modelmats) {
        auto& g = modelmats.template get<e_components::geometry>(e);
        vk::BufferPool::index_t buffer = g.m_descriptor_buffers[2 * static_cast<size_t>(Renderer::DescriptorSetSlot::ModelMatrix) + W];
        memcpy(m_twogame->renderer()->perobject_buffer_pool(Renderer::DescriptorSetSlot::ModelMatrix)->buffer_memory(buffer),
            modelmats.template get<e_components::transform>(e).raw,
            sizeof(mat4));
    }

    auto skels = m_registry.view<e_components::joint_mats, e_components::geometry, JMD_t>();
    for (entt::entity e : skels) {
        auto& g = skels.template get<e_components::geometry>(e);
        vk::BufferPool::index_t buffer = g.m_descriptor_buffers[2 * static_cast<size_t>(Renderer::DescriptorSetSlot::BoneMatrices) + W];
        memcpy(m_twogame->renderer()->perobject_buffer_pool(Renderer::DescriptorSetSlot::BoneMatrices)->buffer_memory(buffer),
            skels.template get<e_components::joint_mats>(e).m_mats.data(),
            skels.template get<e_components::joint_mats>(e).m_mats.size() * sizeof(mat4));
    }

    auto morphs = m_registry.view<e_components::morph_weights, e_components::geometry, MWD_t>();
    for (entt::entity e : morphs) {
        auto& g = morphs.template get<e_components::geometry>(e);
        vk::BufferPool::index_t buffer = g.m_descriptor_buffers[2 * static_cast<size_t>(Renderer::DescriptorSetSlot::ShapeKeyWeights) + W];
        memcpy(m_twogame->renderer()->perobject_buffer_pool(Renderer::DescriptorSetSlot::ShapeKeyWeights)->buffer_memory(buffer),
            morphs.template get<e_components::morph_weights>(e).m_weights.data(),
            morphs.template get<e_components::morph_weights>(e).m_weights.size() * sizeof(float));
    }

    m_registry.clear<MMD_t>();
    m_registry.clear<JMD_t>();
    m_registry.clear<MWD_t>();
}

}
