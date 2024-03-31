#pragma once
#include <optional>
#include <string_view>
#include <vector>
#include <cglm/struct.h>
#include "xml.h"

namespace twogame::xml {

struct Assets;
namespace assets {
    struct AssetBase {
        XML_FIELD(std::string_view, name);
        XML_FIELD(std::string_view, source);
        AssetBase(const pugi::xml_node&, const Assets&);
    };
    struct Animation : AssetBase {
        struct Output {
            XML_FIELD(std::string_view, target);
            XML_FIELD(uint32_t, bone);
            XML_FIELD(uint32_t, width);
            Output(const pugi::xml_node&);
        };
        XML_FIELD(IntPair, range);
        XML_FIELD(size_t, keyframes);
        XML_FIELD(std::string_view, method);
        XML_FIELD(std::vector<Output>, outputs);
        Animation(const pugi::xml_node&, const Assets&);
    };
    struct Image : AssetBase {
        XML_FIELD(std::string_view, usage);
        XML_FIELD(std::string_view, image_source);
        Image(const pugi::xml_node&, const Assets&);
    };
    struct Material : AssetBase {
        struct Prop {
            XML_FIELD(std::string_view, name);
            XML_FIELD(std::string_view, type);
            XML_FIELD(std::string_view, value);
            Prop(const pugi::xml_node&);
        };
        XML_FIELD(std::string_view, shader);
        XML_FIELD(std::vector<Prop>, props);
        Material(const pugi::xml_node&, const Assets&);
    };
    struct Mesh : AssetBase {
        struct Primitives {
            struct Attributes {
                struct Attribute {
                    XML_FIELD(std::string_view, name);
                    XML_FIELD(std::string_view, format);
                    Attribute(const pugi::xml_node&);
                };
                XML_FIELD(IntPair, range);
                XML_FIELD(bool, interleaved);
                XML_FIELD(std::vector<Attribute>, attributes);
                Attributes(const pugi::xml_node&);
            };
            struct Indexes {
                XML_FIELD(size_t, count);
                XML_FIELD(IntPair, range);
                Indexes(const pugi::xml_node&);
            };
            struct Displacements {
                XML_FIELD(std::string_view, name);
                XML_FIELD(IntPair, range);
                Displacements(const pugi::xml_node&);
            };
            XML_FIELD(std::vector<Attributes>, attributes);
            XML_FIELD(std::optional<Indexes>, indexes);
            XML_FIELD(std::vector<Displacements>, displacements);
            XML_FIELD(size_t, count);
            Primitives(const pugi::xml_node&);
        };
        XML_FIELD(std::string_view, primitive_topology);
        XML_FIELD(std::vector<Primitives>, primitives);
        XML_FIELD(std::vector<float>, shape_weights);
        Mesh(const pugi::xml_node&, const Assets&);
    };
    struct Skeleton : AssetBase {
        struct Joint {
            XML_FIELD(size_t, parent);
            XML_FIELD(vec3s, translation);
            XML_FIELD(versors, orientation);
            Joint(const pugi::xml_node&);
        };
        XML_FIELD(IntPair, range);
        XML_FIELD(std::vector<Joint>, joints);
        Skeleton(const pugi::xml_node&, const Assets&);
    };
    struct Shader : AssetBase {
        struct Stage {
            struct Specialization {
                XML_FIELD(uint32_t, constant_id);
                XML_FIELD(std::string_view, value);
                Specialization(const pugi::xml_node&);
            };
            XML_FIELD(std::string_view, stage);
            XML_FIELD(std::string_view, source);
            XML_FIELD(std::vector<Specialization>, specialization);
            Stage(const pugi::xml_node&);
        };
        XML_FIELD(std::vector<Stage>, stages);
        Shader(const pugi::xml_node&, const Assets&);
    };
}

struct Assets {
    XML_FIELD(std::string, source);
    XML_FIELD(std::vector<assets::Animation>, animations);
    XML_FIELD(std::vector<assets::Image>, images);
    XML_FIELD(std::vector<assets::Material>, materials);
    XML_FIELD(std::vector<assets::Mesh>, meshes);
    XML_FIELD(std::vector<assets::Shader>, shaders);
    XML_FIELD(std::vector<assets::Skeleton>, skeletons);
    Assets(const pugi::xml_node&, const std::string& path);

    static constexpr const char* const root_name() { return "assets"; }
};

}
