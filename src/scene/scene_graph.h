#pragma once
#include <cstdint>
#include <span>
#include <variant>
#include <vector>
#include "core/math.h"

namespace twogame {

class SceneGraph {
    std::vector<uint32_t> m_parents, m_children, m_siblings;
    std::vector<std::variant<mat4s, TRS>> m_local_transforms;
    std::vector<mat4s> m_global_transforms;

public:
    constexpr static uint32_t NONE = std::numeric_limits<uint32_t>::max();

    SceneGraph() { }
    SceneGraph(std::vector<uint32_t>&& parents, std::vector<std::variant<mat4s, TRS>>&& transforms);
    inline size_t node_count() const { return m_parents.size(); }
    inline std::span<const mat4s> global_transforms() const { return m_global_transforms; }
    TRS& transform(uint32_t node);

    /** @return the index of the newly inserted, empty scene node */
    uint32_t insert(uint32_t parent, const std::variant<mat4s, TRS>& xfm);
    /** Inserts an `other` scene graph into this one, preserving node order in both.
     * @return the index of the first node of the inserted scene graph. */
    uint32_t insert(uint32_t parent, const SceneGraph& other);
    void reparent(uint32_t node, uint32_t new_parent);
    /** Erases a node from the scene without modifying existing elements.
     * No memory is reclaimed. */
    void remove(uint32_t node);

    void update_global_transforms();
};

}
