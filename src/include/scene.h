#pragma once
#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <asset.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

class Twogame;

namespace twogame {

class Scene {
    Twogame* m_twogame;
    glm::mat4 m_camera_view;
    asset::AssetManager m_assets;
    std::map<entt::hashed_string::hash_type, entt::entity> m_named_entities;
    entt::registry m_registry;

public:
    Scene(Twogame* tg, std::string_view path);

    inline const glm::mat4& camera_view() const { return m_camera_view; }
    inline size_t prepare_assets(VkCommandBuffer cmd) { return m_assets.prepare(cmd); }
    inline void post_prepare_assets() { return m_assets.post_prepare(); }

    void draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, const std::array<VkDescriptorSet, 3>& descriptor_sets);
    void update_transforms();
};

}
