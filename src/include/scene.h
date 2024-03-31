#pragma once
#include <entt/entity/registry.hpp>
#include <entt/core/hashed_string.hpp>
#include "asset.h"
#include "components.h"

class Twogame;

namespace twogame {

class Scene {
    Twogame* m_twogame;
    mat4s m_camera_view;
    asset::AssetManager m_assets;
    std::map<entt::hashed_string::hash_type, entt::entity> m_named_entities;
    entt::registry m_registry;

    void write_perobject_descriptors(entt::entity, e_components::geometry&);
    template <size_t W>
    void _update_perobject_descriptors();

public:
    Scene(Twogame* tg, std::string_view path);

    inline const mat4s& camera_view() const { return m_camera_view; }
    inline size_t prepare_assets(VkCommandBuffer cmd) { return m_assets.prepare(cmd); }
    inline void post_prepare_assets() { return m_assets.post_prepare(); }

    void animate(uint64_t frame_time, uint64_t delta_time);
    void update_transforms();
    void update_perobject_descriptors();
    void draw(VkCommandBuffer cmd, uint64_t frame_number);
};

}
