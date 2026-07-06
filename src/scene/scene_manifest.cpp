#include "scene_manifest.h"
#include <deque>
#include <physfs.h>
#include "asset.fbs.hpp"
#include "assets/animation.h"
#include "assets/image.h"
#include "assets/material.h"
#include "assets/mesh.h"
#include "assets/skeleton.h"

namespace twogame {

AssetContainer& AssetContainer::operator+=(const AssetContainer& other)
{
    auto p_images = m_images.insert(m_images.end(), other.m_images.begin(), other.m_images.end());
    auto p_materials = m_materials.insert(m_materials.end(), other.m_materials.begin(), other.m_materials.end());
    m_animations.insert(m_animations.end(), other.m_animations.begin(), other.m_animations.end());
    m_meshes.insert(m_meshes.end(), other.m_meshes.begin(), other.m_meshes.end());
    m_skeletons.insert(m_skeletons.end(), other.m_skeletons.begin(), other.m_skeletons.end());

    for (auto it = p_materials; it != m_materials.end(); ++it)
        (*it)->advance_images(std::distance(m_images.begin(), p_images));
    return *this;
}

SceneManifest::SceneManifest(const std::string& path)
{
    PHYSFS_File* fh = PHYSFS_openRead(path.c_str());
    if (fh == nullptr)
        return;

    size_t manifest_size = PHYSFS_fileLength(fh);
    std::shared_ptr<std::byte[]> manifest_data = std::make_shared<std::byte[]>(manifest_size);
    PHYSFS_readBytes(fh, manifest_data.get(), manifest_size);
    PHYSFS_close(fh);
    m_manifest = std::shared_ptr<const fbs::Assets>(manifest_data, fbs::GetAssets(manifest_data.get()));

    m_slurp_buffer = [path_pfx = path.substr(0, path.find_last_of('.'))](size_t i, std::function<void*(size_t)> resize) {
        if (i > 0) {
            char path_buf[512];
            snprintf(path_buf, 512, "%s.%u.bin", path_pfx.c_str(), static_cast<unsigned int>(i));
            PHYSFS_File* sfh = PHYSFS_openRead(path_buf);
            if (sfh == nullptr)
                return false;

            PHYSFS_sint64 size = PHYSFS_fileLength(sfh);
            void* dst = resize(size);
            if (PHYSFS_readBytes(sfh, dst, size) < size)
                return false;
            if (PHYSFS_close(sfh) == 0)
                return false;
        }
        return true;
    };

    for (size_t i = 0; m_manifest->animations() && i < m_manifest->animations()->size(); i++)
        m_container.animations().emplace_back(std::make_shared<asset::Animation>(*this, i, m_container.animations().size()));
    for (size_t i = 0; m_manifest->images() && i < m_manifest->images()->size(); i++)
        m_container.images().emplace_back(std::make_shared<asset::Image>(*this, i, m_container.images().size()));
    for (size_t i = 0; m_manifest->materials() && i < m_manifest->materials()->size(); i++)
        m_container.materials().emplace_back(std::make_shared<asset::Material>(*this, i, m_container.materials().size()));
    for (size_t i = 0; m_manifest->meshes() && i < m_manifest->meshes()->size(); i++)
        m_container.meshes().emplace_back(std::make_shared<asset::Mesh>(*this, i, m_container.meshes().size()));
    for (size_t i = 0; m_manifest->skeletons() && i < m_manifest->skeletons()->size(); i++)
        m_container.skeletons().emplace_back(std::make_shared<asset::Skeleton>(*this, i, m_container.skeletons().size()));

    if (m_manifest->nodes() && m_manifest->roots() && m_manifest->nodes()->size() > 0) {
        std::vector<uint32_t> scene_parents(m_manifest->nodes()->size(), SceneGraph::NONE);
        std::vector<std::variant<mat4s, TRS>> scene_xfm(scene_parents.size(), TRS());
        std::deque<uint32_t> queue(m_manifest->roots()->begin(), m_manifest->roots()->end());
        while (queue.empty() == false) {
            uint32_t node_index = queue.front();
            const fbs::SceneNode* node = m_manifest->nodes()->Get(node_index);
            queue.pop_front();

            for (size_t i = 0; node->children() && i < node->children()->size(); i++) {
                uint32_t child = node->children()->Get(i);
                queue.push_back(child);
                scene_parents[child] = node_index;
            }
            if (node->transform_type() == fbs::Transform::Mat4) {
                mat4s& m = scene_xfm[node_index].emplace<mat4s>();
                memcpy(m.raw, node->transform_as_Mat4(), sizeof(mat4));
            } else if (node->transform_type() == fbs::Transform::TRS) {
                TRS& m = scene_xfm[node_index].emplace<TRS>();
                memcpy(m.translation.raw, node->transform_as_TRS()->translation().v(), sizeof(vec3));
                memcpy(m.rotation.raw, node->transform_as_TRS()->rotation().v(), sizeof(versor));
                memcpy(m.scale.raw, node->transform_as_TRS()->scale().v(), sizeof(vec3));
            }

            if (node->camera() > 0) {
                CameraNode& camera = m_cameras.emplace_back();
                camera.node_index = node_index;
                camera.camera_index = node->camera();
            }
            if (node->mesh()) {
                std::shared_ptr<uint32_t[]> mesh_skin;
                std::shared_ptr<float[]> mesh_weights;
                if (node->skeleton()) {
                    uint32_t node_offset = scene_parents.size();
                    auto skeleton = m_container.skeletons()[node->skeleton().value()];
                    if (node->skin()) {
                        mesh_skin = std::make_shared<uint32_t[]>(node->skin()->size());
                        std::copy(node->skin()->begin(), node->skin()->end(), mesh_skin.get());
                    } else {
                        mesh_skin = std::make_shared<uint32_t[]>(skeleton->joints().size());
                        for (auto it = skeleton->bone_parents().begin(); it != skeleton->bone_parents().end(); ++it) {
                            if (*it == std::numeric_limits<uint32_t>::max()) // the parent of the root of the skeleton
                                scene_parents.push_back(node_index); //         is the mesh node
                            else
                                scene_parents.push_back(*it + node_offset);
                        }
                        for (size_t i = 0; i < skeleton->joints().size(); i++) {
                            mesh_skin[i] = skeleton->joints()[i] + node_offset;
                        }
                        std::copy(skeleton->bone_transforms().begin(), skeleton->bone_transforms().end(), std::back_inserter(scene_xfm));
                    }
                }
                if (node->displace_weights()) {
                    mesh_weights = std::make_shared<float[]>(node->displace_weights()->size());
                    std::copy(node->displace_weights()->begin(), node->displace_weights()->end(), mesh_weights.get());
                } else if (auto manifest_weights = m_manifest->meshes()->Get(node->mesh()->Get(0))->displace_weights()) {
                    mesh_weights = std::make_shared<float[]>(manifest_weights->size());
                    std::copy(manifest_weights->begin(), manifest_weights->end(), mesh_weights.get());
                }
                for (size_t i = 0; i < node->mesh()->size(); i++) {
                    MeshNode& mesh = m_meshes.emplace_back();
                    mesh.node_index = node_index;
                    mesh.mesh_index = node->mesh()->Get(i);
                    mesh.material_index = m_container.meshes()[mesh.mesh_index]->material_index();
                    mesh.skeleton_index = node->skeleton().value_or(std::numeric_limits<uint32_t>::max());
                    mesh.skin = mesh_skin;
                    mesh.weights = mesh_weights;
                }
            }
        }

        m_scenegraph = SceneGraph(std::move(scene_parents), std::move(scene_xfm));
    }
}

}
