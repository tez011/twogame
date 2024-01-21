#pragma once
#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <asset.h>
#include <entt/entt.hpp>

class Twogame;

namespace twogame {

class Scene {
    Twogame* m_twogame;
    asset::AssetManager m_assets;
    entt::registry m_registry;

public:
    Scene(Twogame* tg, std::string_view path);

    inline size_t prepare_assets(VkCommandBuffer cmd) { return m_assets.prepare(cmd); }
    inline void post_prepare_assets() { return m_assets.post_prepare(); }
    void draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, const std::array<VkDescriptorSet, 3>& descriptor_sets);
};

}
