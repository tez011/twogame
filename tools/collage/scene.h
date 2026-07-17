#pragma once
#include <map>
#include <ranges>
#include <span>
#include <string>
#include "scene.fbs.hpp"

namespace fbs = twogame::fbs;

class OutputScene {
    fbs::SceneT m_scene;
    std::string m_asset_path;

    size_t push_ibuffer(std::span<const std::byte> data);

public:
    std::map<std::string, uint32_t, std::less<>> animation_names, image_names, material_names, node_names;
    std::map<std::string, std::vector<uint32_t>, std::less<>> mesh_names;

    OutputScene(const std::string& asset_path);
    fbs::SceneT* operator->() { return &m_scene; }

    size_t count() const { return m_scene.buffers.size(); }
    size_t size(size_t index) const { return m_scene.buffers[index]; }

    template <std::ranges::contiguous_range T>
    size_t push_buffer(T&& data)
    {
        if (data.empty())
            return 0;
        else
            return push_ibuffer(std::as_bytes(std::span(data)));
    }

    void finish();
};
