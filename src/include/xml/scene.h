#pragma once
#include <string_view>
#include <variant>
#include <cglm/struct.h>
#include "xml.h"

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
