#include "scene_graph.h"
#include <algorithm>

namespace twogame {

SceneGraph::SceneGraph(std::vector<uint32_t>&& parents, std::vector<std::variant<mat4s, TRS>>&& transforms)
{
    m_parents = std::move(parents);
    m_children.assign(m_parents.size(), NONE);
    m_siblings.assign(m_parents.size(), NONE);
    m_local_transforms = std::move(transforms);
    for (size_t i = 0; i < node_count(); i++) {
        uint32_t parent = m_parents[i];
        if (parent != NONE) {
            m_siblings[i] = m_children[parent];
            m_children[parent] = i;
        }
    }
}

TRS& SceneGraph::transform(uint32_t node)
{
    if (TRS* m = std::get_if<TRS>(&m_local_transforms[node])) {
        return *m;
    } else {
        return m_local_transforms[node].emplace<TRS>();
    }
}

uint32_t SceneGraph::insert(uint32_t parent, const std::variant<mat4s, TRS>& xfm)
{
    uint32_t new_index = m_parents.size();
    m_siblings.push_back(m_children[parent]);
    m_children.push_back(NONE);
    m_parents.push_back(parent);
    m_children[parent] = new_index;
    return new_index;
}

uint32_t SceneGraph::insert(uint32_t parent, const SceneGraph& other)
{
    uint32_t other_start = m_parents.size(), new_size = m_parents.size() + other.m_parents.size();
    m_siblings.reserve(new_size);
    m_children.reserve(new_size);
    m_parents.reserve(new_size);

    std::vector<uint32_t> root_siblings;
    for (size_t i = 0; i < other.m_parents.size(); i++) {
        if (other.m_parents[i] == NONE) {
            m_parents.push_back(parent);
            root_siblings.push_back(i + other_start);
        } else {
            m_parents.push_back(other.m_parents[i] + other_start);
        }
    }
    root_siblings.push_back(m_children[parent]);

    auto transform_cs_node_index = [other_start](uint32_t j) { return j == NONE ? NONE : j + other_start; };
    std::transform(other.m_children.begin(), other.m_children.end(), std::back_inserter(m_children), transform_cs_node_index);
    std::transform(other.m_siblings.begin(), other.m_siblings.end(), std::back_inserter(m_siblings), transform_cs_node_index);

    for (int i = 0; i < int(root_siblings.size()) - 1; i++)
        m_siblings[root_siblings[i]] = root_siblings[i + 1];
    m_children[parent] = root_siblings[0];
    return other_start;
}

void SceneGraph::reparent(uint32_t node, uint32_t new_parent)
{
    uint32_t old_parent = m_parents[node];
    if (node == new_parent || old_parent == new_parent)
        return;

    if (old_parent != NONE) {
        // Remove the current node from its parent's child chain
        uint32_t n = m_children[old_parent], prev = NONE;
        while (n != NONE && n != node) {
            prev = n;
            n = m_siblings[n];
        }
        if (n == node) {
            if (prev == NONE) {
                // This node was the first child.
                m_children[old_parent] = m_siblings[node];
            } else {
                m_siblings[prev] = m_siblings[node];
            }
        }
    }

    m_parents[node] = new_parent;
    if (new_parent == NONE) {
        // root nodes have no siblings
        m_siblings[node] = NONE;
    } else {
        m_siblings[node] = m_children[new_parent];
        m_children[new_parent] = node;
    }
}

void SceneGraph::remove(uint32_t node)
{
    if (m_children[m_parents[node]] == node)
        m_children[m_parents[node]] = m_siblings[node];
    for (uint32_t c = m_children[m_parents[node]]; c != NONE; c = m_siblings[c]) {
        if (m_siblings[c] == node)
            m_siblings[c] = m_siblings[node];
    }
}

void SceneGraph::update_global_transforms()
{
    std::vector<mat4s> local_mats(node_count());
    std::vector<uint32_t> stack;
    m_global_transforms.resize(node_count());
    stack.reserve(64);

    for (size_t i = 0; i < node_count(); i++) {
        local_mats[i] = std::visit([](auto&& xfm) -> mat4s {
            using T = std::decay_t<decltype(xfm)>;
            if constexpr (std::is_same<T, mat4s>::value)
                return xfm;
            else if constexpr (std::is_same<T, TRS>::value)
                return xfm.transform_matrix();
        },
            m_local_transforms[i]);

        if (m_parents[i] == NONE) {
            m_global_transforms[i] = local_mats[i];
            stack.push_back(i);
        }
    }
    while (stack.empty() == false) {
        uint32_t node = stack.back();
        stack.pop_back();

        for (uint32_t child = m_children[node]; child != NONE; child = m_siblings[child]) {
            glm_mat4_mul(m_global_transforms[node].raw, local_mats[child].raw, m_global_transforms[child].raw);
            stack.push_back(child);
        }
    }
}

}
