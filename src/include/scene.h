#pragma once
#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <asset.h>
#include <entt/entt.hpp>

class Twogame;

template <typename... Bases>
struct overload : Bases... {
    using is_transparent = void;
    using Bases::operator()...;
};

namespace twogame {

class Scene {
    template <typename T>
    class lookup : public std::unordered_map<std::string, T, overload<std::hash<std::string>, std::hash<std::string_view>>, std::equal_to<>> { };

    Twogame* m_twogame;
    entt::registry m_registry;
    lookup<std::shared_ptr<asset::Image>> m_images;
    lookup<std::shared_ptr<asset::Mesh>> m_meshes;
    lookup<std::shared_ptr<asset::Shader>> m_shaders;
    lookup<VkDescriptorSet> m_materials;
    std::deque<asset::AbstractAsset*> m_assets_preparing;

    bool import_assets(std::string_view path);

public:
    Scene(Twogame* tg, std::string_view path);

    size_t prepare_assets(VkCommandBuffer cmd);
    void post_prepare_assets();

    void draw(VkCommandBuffer cmd, VkRenderPass render_pass, uint32_t subpass, const std::array<VkDescriptorSet, 4>& descriptor_sets);
};

}
