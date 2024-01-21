#include <physfs.h>
#include "asset.h"
#include "xml.h"

namespace twogame::asset {

bool AssetManager::import_assets(std::string_view path, const Renderer* renderer)
{
    xml::Document<xml::Assets> assetdoc(path);
    if (assetdoc.ok() == false)
        return false;

    try {
        for (auto& s : assetdoc->shaders())
            m_shaders[std::string { s.name() }] = std::make_shared<asset::Shader>(s, renderer);
        for (auto& i : assetdoc->images())
            m_images[std::string { i.name() }] = std::make_shared<asset::Image>(i, renderer);
        for (auto& m : assetdoc->meshes())
            m_meshes[std::string { m.name() }] = std::make_shared<asset::Mesh>(m, renderer);

        for (auto& m : assetdoc->materials()) {
            auto shader = m_shaders.find(m.shader());
            if (shader == m_shaders.end()) {
                spdlog::error("failed to load material {}: shader {} not found", m.name(), m.shader());
                return false;
            } else
                m_materials[std::string { m.name() }] = std::make_shared<asset::Material>(m, *this);
        }
    } catch (asset::IOException& e) {
        spdlog::error("failed to load asset {}: {}", e.path(), PHYSFS_getErrorByCode((PHYSFS_ErrorCode)e.errcode()));
        return false;
    } catch (asset::MalformedException& e) {
        spdlog::error("malformed asset {}: {}", e.name(), e.description());
        return false;
    }
    return true;
}

size_t AssetManager::prepare(VkCommandBuffer cmd)
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

void AssetManager::post_prepare()
{
    while (m_assets_preparing.empty() == false) {
        m_assets_preparing.front()->post_prepare();
#ifdef TWOGAME_DEBUG_BUILD
        assert(m_assets_preparing.front()->prepared());
#endif

        m_assets_preparing.pop_front();
    }
}

}
