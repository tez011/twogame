#include "scene.h"
#include <list>
#include <ranges>
#include <physfs.h>
#include "components.h"
#include "render.h"
#include "twogame.h"
#include "xml.h"

using namespace std::string_view_literals;

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
                if (strcmp(fullpath.c_str() + fullpath.length() - 4, ".xml") == 0 && !import_assets(fullpath)) {
                    spdlog::error("failed to import assets at {}", fullpath);
                }
            }
        } else if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
            if (!import_assets(asset_path)) {
                spdlog::error("failed to import assets at {}", asset_path);
            }
        }
    }

    std::vector<VkWriteDescriptorSet> writes;
    for (const auto& minfo : scenedoc->materials()) {
        auto shader_it = m_shaders.find(minfo.shader());
        if (shader_it == m_shaders.end()) {
            spdlog::error("material {}: unknown shader {}", minfo.name(), minfo.shader());
            continue;
        }

        auto& shader = shader_it->second;
        VkDescriptorSet material_descriptor_set;
        shader->material_descriptor_pool()->allocate(&material_descriptor_set);
        m_materials[std::string { minfo.name() }] = material_descriptor_set;
        for (auto it = minfo.props().begin(); it != minfo.props().end(); ++it) {
            auto mbi = shader->material_bindings().find(it->first);
            if (mbi == shader->material_bindings().end()) {
                spdlog::warn("skipping unpaired material entry '{}'", it->first);
                continue;
            }

            auto& sdss = mbi->second;
            if (sdss.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                std::vector<std::string_view> image_names;
                auto image_info = new VkDescriptorImageInfo[sdss.count];
                xml::priv::split(image_names, it->second, " ");
                if (image_names.size() != sdss.count) {
                    spdlog::error("descriptor {} has count={} but {} were given", it->first, sdss.count, image_names.size());
                    continue;
                }

                for (uint32_t i = 0; i < sdss.count; i++) {
                    auto ii = m_images.find(image_names[i]);
                    image_info[i].sampler = m_twogame->renderer()->active_sampler();
                    image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    if (ii == m_images.end()) {
                        spdlog::error("unknown image {} referenced in material", image_names[i]);
                        image_info[i].imageView = VK_NULL_HANDLE;
                    } else {
                        image_info[i].imageView = static_cast<asset::Image*>(ii->second.get())->image_view();
                    }
                }

                VkWriteDescriptorSet& w = writes.emplace_back();
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = material_descriptor_set;
                w.dstBinding = sdss.binding;
                w.dstArrayElement = 0;
                w.descriptorCount = sdss.count;
                w.descriptorType = sdss.type;
                w.pImageInfo = image_info;
            }
        }
    }
    vkUpdateDescriptorSets(m_twogame->renderer()->device(), writes.size(), writes.data(), 0, nullptr);
    for (VkWriteDescriptorSet& w : writes) {
        if (w.pImageInfo != nullptr)
            delete[] w.pImageInfo;
    }

    for (const auto& einfo : scenedoc->entities()) {
        const auto entity = m_registry.create();
        if (einfo.rigidbody()) {
            m_registry.emplace<e_components::translation>(entity, einfo.rigidbody()->translation());
            m_registry.emplace<e_components::orientation>(entity, einfo.rigidbody()->orientation());
        }
        if (einfo.geometry()) {
            auto mesh_it = m_meshes.find(einfo.geometry()->mesh());
            auto material_it = m_materials.find(einfo.geometry()->material());
            auto shader_it = m_shaders.find(einfo.geometry()->shader());
            if (mesh_it == m_meshes.end()) {
                spdlog::error("unknown mesh {}", einfo.geometry()->mesh());
                continue;
            }
            if (material_it == m_materials.end()) {
                spdlog::error("unknown material {}", einfo.geometry()->material());
                continue;
            }
            if (shader_it == m_shaders.end()) {
                spdlog::error("unknown shader {}", einfo.geometry()->shader());
                continue;
            }

            std::map<std::pair<VkRenderPass, uint32_t>, VkPipeline> pipelines;
            for (auto it = shader_it->second->graphics_pipelines().begin(); it != shader_it->second->graphics_pipelines().end(); ++it)
                pipelines[it->first] = tg->renderer()->pipeline_factory().graphics_pipeline(mesh_it->second->vertex_input_state(), it->second);

            m_registry.emplace<e_components::geometry>(entity, mesh_it->second, shader_it->second, material_it->second, pipelines);
        }
    }
}

bool Scene::import_assets(std::string_view path)
{
    xml::Document<xml::Assets> assetdoc(path);
    if (assetdoc.ok() == false)
        return false;

    try {
        for (auto& i : assetdoc->images())
            m_images[std::string { i.name() }] = std::make_shared<asset::Image>(i, m_twogame->renderer());
        for (auto& m : assetdoc->meshes())
            m_meshes[std::string { m.name() }] = std::make_shared<asset::Mesh>(m, m_twogame->renderer());
        for (auto& s : assetdoc->shaders())
            m_shaders[std::string { s.name() }] = std::make_shared<asset::Shader>(s, m_twogame->renderer());
    } catch (asset::IOException& e) {
        spdlog::error("failed to load asset {}: {}", e.path(), PHYSFS_getErrorByCode((PHYSFS_ErrorCode)e.errcode()));
        return false;
    } catch (asset::MalformedException& e) {
        spdlog::error("malformed asset {}: {}", e.name(), e.description());
        return false;
    }
    return true;
}

size_t Scene::prepare_assets(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo cb_begin {};
    cb_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cb_begin);

#define PREPARE_FROM(CONTAINER_NAME)                                                       \
    do {                                                                                   \
        for (auto it = m_##CONTAINER_NAME.begin(); it != m_##CONTAINER_NAME.end(); ++it) { \
            if (it->second->prepared() == false) {                                         \
                it->second->prepare(cmd);                                                  \
                m_assets_preparing.push_back(it->second.get());                            \
            }                                                                              \
        }                                                                                  \
    } while (0)

    PREPARE_FROM(images);
    PREPARE_FROM(meshes);
    // PREPARE_FROM(m_shaders); // shaders don't need preparation.

    vkEndCommandBuffer(cmd);
    return m_assets_preparing.size();
}

void Scene::post_prepare_assets()
{
    while (m_assets_preparing.empty() == false) {
        m_assets_preparing.front()->post_prepare();
#ifdef TWOGAME_DEBUG_BUILD
        assert(m_assets_preparing.front()->prepared());
#endif

        m_assets_preparing.pop_front();
    }
}

void Scene::draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, const std::array<VkDescriptorSet, 4>& o_descriptor_sets)
{
    auto descriptor_sets = o_descriptor_sets;
    auto view = m_registry.view<e_components::geometry>();

    for (entt::entity e : view) {
        auto& g = view.get<e_components::geometry>(e);
        glm::mat4 modelmat(1.f);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_pipelines[std::make_pair(render_pass, subpass)]);

        descriptor_sets[3] = g.m_material;
        vkCmdPushConstants(cmd, g.m_shader->pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &modelmat);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_shader->pipeline_layout(),
            0, descriptor_sets.size(), descriptor_sets.data(),
            0, nullptr);

        g.m_mesh->bind_buffers(cmd);
        vkCmdDrawIndexed(cmd, g.m_mesh->index_count(), 1, 0, 0, 0);
    }
}

}
