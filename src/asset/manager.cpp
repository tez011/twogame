#define XML_DEFINE_DOCUMENT
#include <filesystem>
#include <physfs.h>
#include "asset.h"
#include "xml/asset.h"

namespace twogame::asset {

AssetManager::AssetManager(const Renderer* r)
    : m_renderer(*r)
{
}

bool AssetManager::import_assets(std::string_view path)
{
    xml::Document<xml::Assets> assetdoc(path);
    if (assetdoc.ok() == false)
        return false;

    try {
        for (const auto& s : assetdoc->shaders())
            m_shaders[std::string { s.name() }] = std::make_shared<asset::Shader>(s, *this);
        for (const auto& i : assetdoc->images())
            m_images[std::string { i.name() }] = std::make_shared<asset::Image>(i, *this);
        for (const auto& m : assetdoc->materials())
            m_materials[std::string { m.name() }] = std::make_shared<asset::Material>(m, *this); // depends on shaders
        for (const auto& m : assetdoc->meshes())
            m_meshes[std::string { m.name() }] = std::make_shared<asset::Mesh>(m, *this);
        for (const auto& a : assetdoc->skeletons())
            m_skeletons[std::string { a.name() }] = std::make_shared<asset::Skeleton>(a, *this);
        for (const auto& a : assetdoc->animations())
            m_animations[std::string { a.name() }] = std::make_shared<asset::Animation>(a, *this);
    } catch (asset::IOException& e) {
        spdlog::error("failed to load asset data at {}: {}", e.path(), PHYSFS_getErrorByCode((PHYSFS_ErrorCode)e.errcode()));
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

    for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        if (it->second->prepared() == false) {
            it->second->prepare(cmd);
            m_assets_preparing.push_back(it->second.get());
        }
    }
    for (auto it = m_meshes.begin(); it != m_meshes.end(); ++it) {
        if (it->second->prepared() == false) {
            it->second->prepare(cmd);
            m_assets_preparing.push_back(it->second.get());
        }
    }

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
