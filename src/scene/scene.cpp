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
                if (strcmp(fullpath.c_str() + fullpath.length() - 4, ".xml") == 0 && !m_assets.import_assets(fullpath, tg->renderer())) {
                    spdlog::error("failed to import assets at {}", fullpath);
                }
            }
        } else if (stat.filetype == PHYSFS_FILETYPE_REGULAR) {
            if (!m_assets.import_assets(asset_path, m_twogame->renderer())) {
                spdlog::error("failed to import assets at {}", asset_path);
            }
        }
    }

    for (const auto& einfo : scenedoc->entities()) {
        const auto entity = m_registry.create();
        if (einfo.rigidbody()) {
            m_registry.emplace<e_components::translation>(entity, einfo.rigidbody()->translation());
            m_registry.emplace<e_components::orientation>(entity, einfo.rigidbody()->orientation());
        }
        if (einfo.geometry()) {
            auto mesh_it = m_assets.meshes().find(einfo.geometry()->mesh());
            if (mesh_it == m_assets.meshes().end()) {
                spdlog::error("unknown mesh {}", einfo.geometry()->mesh());
                continue;
            }

            std::shared_ptr<asset::Material> material;
            if (einfo.geometry()->material()->name().empty()) {
                auto shader_it = m_assets.shaders().find(einfo.geometry()->material()->shader());
                if (shader_it == m_assets.shaders().end()) {
                    spdlog::error("unknown shader {}", einfo.geometry()->material()->shader());
                    continue;
                }

                material = std::make_shared<asset::Material>(einfo.geometry()->material().value(), m_assets);
            } else {
                auto material_it = m_assets.materials().find(einfo.geometry()->material()->name());
                if (material_it == m_assets.materials().end()) {
                    spdlog::error("unknown material {}", einfo.geometry()->material()->name());
                    continue;
                }
                if (einfo.geometry()->material()->unique())
                    material = std::make_shared<asset::Material>(*material_it->second.get());
                else
                    material = material_it->second;
            }

            (void)material->shader()->graphics_pipeline(mesh_it->second.get());
            m_registry.emplace<e_components::geometry>(entity, mesh_it->second, material);
        }
    }
}

void Scene::draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, const std::array<VkDescriptorSet, 3>& in_descriptor_sets)
{
    auto view = m_registry.view<e_components::geometry>();
    std::array<VkDescriptorSet, 4> descriptor_sets;
    std::copy(in_descriptor_sets.begin(), in_descriptor_sets.end(), descriptor_sets.begin());

    for (entt::entity e : view) {
        auto& g = view.get<e_components::geometry>(e);
        glm::mat4 modelmat(1.f);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->graphics_pipeline(g.m_mesh.get()));

        descriptor_sets[3] = g.m_material->descriptor();
        vkCmdPushConstants(cmd, g.m_material->shader()->pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &modelmat);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.m_material->shader()->pipeline_layout(),
            0, descriptor_sets.size(), descriptor_sets.data(),
            0, nullptr);

        g.m_mesh->bind_buffers(cmd);
        vkCmdDrawIndexed(cmd, g.m_mesh->index_count(), 1, 0, 0, 0);
    }
}

}
