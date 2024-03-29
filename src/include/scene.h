#pragma once
#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <entt/entt.hpp>
#include "asset.h"
#include "components.h"
#include "xml.h"

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

namespace twogame::xml {

struct Scene {
    struct Entity {
        struct Camera {
            Camera(const pugi::xml_node&) { }
        };
        struct Geometry {
            struct BoundMaterial {
                XML_FIELD(std::string_view, name);
                XML_FIELD(bool, immutable);
                BoundMaterial(const pugi::xml_node&);
            };
            XML_FIELD(std::string_view, mesh);
            XML_FIELD(std::string_view, skeleton);
            XML_FIELD(std::vector<BoundMaterial>, materials);
            Geometry(const pugi::xml_node&);
        };
        struct Rigidbody {
            XML_FIELD(bool, physics);
            XML_FIELD(vec3s, translation);
            XML_FIELD(versors, orientation);
            Rigidbody(const pugi::xml_node&);
        };
        struct Animator {
            XML_FIELD(std::string_view, initial_animation);
            Animator(const pugi::xml_node&);
        };
        using EntityComponent = std::variant<Geometry, Camera, Rigidbody, Animator>;

        XML_FIELD(std::string_view, name);
        XML_FIELD(std::string_view, parent);
        XML_FIELD(std::vector<EntityComponent>, components);
        Entity(const pugi::xml_node&);
    };
    XML_FIELD(std::vector<std::string_view>, assets);
    XML_FIELD(std::vector<Entity>, entities);
    Scene(const pugi::xml_node&, const std::string& path);

    static constexpr const char* const root_name() { return "scene"; }
};

}
