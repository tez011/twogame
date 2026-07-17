#include <algorithm>
#include <bitset>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <stack>
#include <vector>
#include <fastgltf/tools.hpp>
#include "scene.h"

namespace fbs = twogame::fbs;
using namespace fastgltf::math;

size_t max_uv_channels = 2, max_color_channels = 1, max_joint_channels = 1;

extern std::vector<std::byte> generate_image(std::span<const std::byte> image_data, bool is_vector_image);

static void load_materials(OutputScene& oscene, const std::vector<fastgltf::Material>& materials, const std::vector<fastgltf::Texture>& textures)
{
    for (auto it = materials.begin(); it != materials.end(); ++it) {
        if (it->name.size() > 0)
            oscene.material_names[std::string(it->name)] = oscene->materials.size();

        auto& ot = oscene->materials.emplace_back(std::make_unique<fbs::MaterialT>());
        ot->alpha_cutoff = it->alphaCutoff;
        ot->alpha_mask = it->alphaMode == fastgltf::AlphaMode::Mask;
        ot->base_color_factor = std::make_unique<fbs::Vec4>(std::span<const float, 4> { it->pbrData.baseColorFactor.data(), 4 });
        ot->double_sided = it->doubleSided;
        ot->metallic_factor = it->pbrData.metallicFactor;
        ot->roughness_factor = it->pbrData.roughnessFactor;
        ot->unlit = it->unlit;
        if (it->pbrData.baseColorTexture) {
            auto& texture = textures[it->pbrData.baseColorTexture.value().textureIndex];
            ot->base_color_texture = std::make_unique<fbs::TextureT>();
            ot->base_color_texture->image = texture.imageIndex.value_or(0);
            ot->base_color_texture->sampler = texture.samplerIndex.value_or(0);
            ot->base_color_uv = it->pbrData.baseColorTexture.value().texCoordIndex;
        }
        if (it->emissiveTexture) {
            auto& texture = textures[it->emissiveTexture.value().textureIndex];
            ot->emissive_texture = std::make_unique<fbs::TextureT>();
            ot->emissive_texture->image = texture.imageIndex.value_or(0);
            ot->emissive_texture->sampler = texture.samplerIndex.value_or(0);
            ot->emissive_uv = it->emissiveTexture.value().texCoordIndex;
        }
        if (it->pbrData.metallicRoughnessTexture) {
            auto& texture = textures[it->pbrData.metallicRoughnessTexture.value().textureIndex];
            ot->metallic_roughness_texture = std::make_unique<fbs::TextureT>();
            ot->metallic_roughness_texture->image = texture.imageIndex.value_or(0);
            ot->metallic_roughness_texture->sampler = texture.samplerIndex.value_or(0);
            ot->metallic_roughness_uv = it->pbrData.metallicRoughnessTexture.value().texCoordIndex;
        }
        if (it->normalTexture) {
            auto& texture = textures[it->normalTexture.value().textureIndex];
            ot->normal_texture = std::make_unique<fbs::TextureT>();
            ot->normal_texture->image = texture.imageIndex.value_or(0);
            ot->normal_texture->sampler = texture.samplerIndex.value_or(0);
            ot->normal_uv = it->normalTexture.value().texCoordIndex;
        }
        if (it->occlusionTexture) {
            auto& texture = textures[it->occlusionTexture.value().textureIndex];
            ot->occlusion_texture = std::make_unique<fbs::TextureT>();
            ot->occlusion_texture->image = texture.imageIndex.value_or(0);
            ot->occlusion_texture->sampler = texture.samplerIndex.value_or(0);
            ot->occlusion_uv = it->occlusionTexture.value().texCoordIndex;
        }

        fvec3 emissive_factor = it->emissiveFactor * it->emissiveStrength;
        ot->emissive_factor = std::make_unique<fbs::Vec3>(std::span<const float, 3> { emissive_factor.data(), emissive_factor.size() });
    }
}

template <typename FindAttribute>
    requires std::invocable<FindAttribute, std::string_view> && std::same_as<std::invoke_result_t<FindAttribute, std::string_view>, const fastgltf::Attribute*>
static void load_mesh_buffers(const fastgltf::Asset& asset, std::span<fvec4> position, std::span<fvec4> normal, size_t uv_channels, size_t color_channels, FindAttribute&& find_attribute)
{
    std::span<fvec2> uv(reinterpret_cast<fvec2*>(position.data()), position.size() * 2);
    fastgltf::iterateAccessorWithIndex<fvec3>(asset, asset.accessors[find_attribute("POSITION")->accessorIndex], [&](fvec3 value, size_t index) {
        position[index * ((uv_channels + 3) / 2)][0] = value[0];
        position[index * ((uv_channels + 3) / 2)][1] = value[1];
        position[index * ((uv_channels + 3) / 2)][2] = value[2];
        position[index * ((uv_channels + 3) / 2)][3] = 1.f;
    });

    const fastgltf::Attribute* attribute;
    char accessor_name[16];
    for (size_t i = 0; i < uv_channels; i++) {
        snprintf(accessor_name, 16, "TEXCOORD_%zu", i);
        if ((attribute = find_attribute(accessor_name)) == nullptr)
            break;

        auto& accessor = asset.accessors[attribute->accessorIndex];
        if (accessor.componentType == fastgltf::ComponentType::Byte) {
            fastgltf::iterateAccessorWithIndex<s8vec2>(asset, accessor, [&](s8vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][0] = std::clamp(value[0] / 127.f, -1.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][1] = std::clamp(value[1] / 127.f, -1.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
            fastgltf::iterateAccessorWithIndex<u8vec2>(asset, accessor, [&](u8vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][0] = std::clamp(value[0] / 255.f, 0.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][1] = std::clamp(value[1] / 255.f, 0.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::Short) {
            fastgltf::iterateAccessorWithIndex<s16vec2>(asset, accessor, [&](s16vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][0] = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][1] = std::clamp(value[1] / 32767.f, -1.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
            fastgltf::iterateAccessorWithIndex<u16vec2>(asset, accessor, [&](u16vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][0] = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][1] = std::clamp(value[1] / 65535.f, 0.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::Float) {
            fastgltf::iterateAccessorWithIndex<fvec2>(asset, accessor, [&](fvec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][0] = value[0];
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i][1] = value[1];
            });
        } else {
            std::unreachable();
        }
    }

    if ((attribute = find_attribute("NORMAL")) != nullptr) {
        fastgltf::iterateAccessorWithIndex<fvec3>(asset, asset.accessors[attribute->accessorIndex], [&](fvec3 value, size_t index) {
            normal[index * (2 + color_channels) + 0][0] = value[0];
            normal[index * (2 + color_channels) + 0][1] = value[1];
            normal[index * (2 + color_channels) + 0][2] = value[2];
            normal[index * (2 + color_channels) + 0][3] = 0.f;
        });
    }
    if ((attribute = find_attribute("TANGENT")) != nullptr) {
        fastgltf::iterateAccessorWithIndex<fvec4>(asset, asset.accessors[attribute->accessorIndex], [&](fvec4 value, size_t index) {
            normal[index * (2 + color_channels) + 1][0] = value[0];
            normal[index * (2 + color_channels) + 1][1] = value[1];
            normal[index * (2 + color_channels) + 1][2] = value[2];
            normal[index * (2 + color_channels) + 1][3] = value[3];
        });
    }
    for (size_t i = 0; i < color_channels; i++) {
        snprintf(accessor_name, 16, "COLOR_%zu", i);
        if ((attribute = find_attribute(accessor_name)) == nullptr)
            break;

        auto& accessor = asset.accessors[attribute->accessorIndex];
        if (accessor.type == fastgltf::AccessorType::Vec3) {
            if (accessor.componentType == fastgltf::ComponentType::Byte) {
                fastgltf::iterateAccessorWithIndex<s8vec3>(asset, accessor, [&](s8vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                fastgltf::iterateAccessorWithIndex<u8vec3>(asset, accessor, [&](u8vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Short) {
                fastgltf::iterateAccessorWithIndex<s16vec3>(asset, accessor, [&](s16vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                fastgltf::iterateAccessorWithIndex<u16vec3>(asset, accessor, [&](u16vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Float) {
                fastgltf::iterateAccessorWithIndex<fvec3>(asset, accessor, [&](fvec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = value[0];
                    normal[index * (2 + color_channels) + 2 + i][1] = value[1];
                    normal[index * (2 + color_channels) + 2 + i][2] = value[2];
                    normal[index * (2 + color_channels) + 2 + i][3] = 0;
                });
            } else {
                std::unreachable();
            }
        } else if (accessor.type == fastgltf::AccessorType::Vec4) {
            if (accessor.componentType == fastgltf::ComponentType::Byte) {
                fastgltf::iterateAccessorWithIndex<s8vec4>(asset, accessor, [&](s8vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = std::clamp(value[3] / 127.f, -1.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                fastgltf::iterateAccessorWithIndex<u8vec4>(asset, accessor, [&](u8vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = std::clamp(value[3] / 255.f, 0.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Short) {
                fastgltf::iterateAccessorWithIndex<s16vec4>(asset, accessor, [&](s16vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = std::clamp(value[3] / 32767.f, -1.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                fastgltf::iterateAccessorWithIndex<u16vec4>(asset, accessor, [&](u16vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][1] = std::clamp(value[1] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][2] = std::clamp(value[2] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i][3] = std::clamp(value[3] / 65535.f, 0.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Float) {
                fastgltf::iterateAccessorWithIndex<fvec4>(asset, accessor, [&](fvec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i][0] = value[0];
                    normal[index * (2 + color_channels) + 2 + i][1] = value[1];
                    normal[index * (2 + color_channels) + 2 + i][2] = value[2];
                    normal[index * (2 + color_channels) + 2 + i][3] = value[3];
                });
            } else {
                std::unreachable();
            }
        } else {
            std::unreachable();
        }
    }
}

static std::vector<std::vector<uint32_t>> load_meshes(OutputScene& oscene, const fastgltf::Asset& asset, size_t uv_channels, size_t color_channels, size_t joint_channels)
{
    std::vector<std::vector<uint32_t>> mesh_groups;
    for (auto ht = asset.meshes.begin(); ht != asset.meshes.end(); ++ht) {
        mesh_groups.emplace_back();
        for (auto it = ht->primitives.begin(); it != ht->primitives.end(); ++it) {
            mesh_groups.back().push_back(oscene->meshes.size());

            auto& ot = oscene->meshes.emplace_back(std::make_unique<fbs::MeshT>());
            auto& indexes_accessor = asset.accessors[it->indicesAccessor.value()];
            ot->index_count = indexes_accessor.count;
            ot->vertex_count = asset.accessors[it->findAttribute("POSITION")->accessorIndex].count;
            if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                std::vector<uint16_t> indexes(ot->index_count);
                fastgltf::iterateAccessorWithIndex<uint8_t>(asset, indexes_accessor, [&](uint8_t value, size_t index) {
                    indexes[index] = value;
                });
                ot->indexes = oscene.push_buffer(indexes);
            } else if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                std::vector<uint16_t> indexes(ot->index_count);
                fastgltf::copyFromAccessor<uint16_t>(asset, indexes_accessor, indexes.data());
                ot->indexes = oscene.push_buffer(indexes);
            } else if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedInt) {
                std::vector<uint32_t> indexes(ot->index_count);
                fastgltf::copyFromAccessor<uint32_t>(asset, indexes_accessor, indexes.data());
                ot->indexes = oscene.push_buffer(indexes);
            } else {
                std::unreachable();
            }

            size_t posn_bsize = (uv_channels + 3) / 2, norm_bsize = 2 + color_channels;
            std::vector<fvec4> positions(ot->vertex_count * posn_bsize), normals(ot->vertex_count * norm_bsize);
            load_mesh_buffers(asset, positions, normals, uv_channels, color_channels, [it](std::string_view name) {
                const fastgltf::Attribute* attr = it->findAttribute(name);
                return attr == it->attributes.end() ? nullptr : attr;
            });

            if (it->findAttribute("JOINTS_0") != it->attributes.end()) {
                std::vector<fvec4> joints_data(ot->vertex_count * joint_channels * 2);
                const fastgltf::Attribute* attribute;
                char accessor_name[16];
                ivec4* joints_index_data = reinterpret_cast<ivec4*>(joints_data.data());
                for (size_t i = 0; i < joint_channels; i++) {
                    snprintf(accessor_name, 16, "JOINTS_%zu", i);
                    if ((attribute = it->findAttribute(accessor_name)) == it->attributes.end())
                        break;

                    fastgltf::iterateAccessorWithIndex<ivec4>(asset, asset.accessors[attribute->accessorIndex], [&](ivec4 value, size_t index) {
                        joints_index_data[index * 2 * joint_channels + i][0] = value[0];
                        joints_index_data[index * 2 * joint_channels + i][1] = value[1];
                        joints_index_data[index * 2 * joint_channels + i][2] = value[2];
                        joints_index_data[index * 2 * joint_channels + i][3] = value[3];
                    });

                    snprintf(accessor_name, 16, "WEIGHTS_%zu", i);
                    fastgltf::iterateAccessorWithIndex<fvec4>(asset, asset.accessors[it->findAttribute(accessor_name)->accessorIndex], [&](fvec4 value, size_t index) {
                        joints_data[index * 2 * joint_channels + joint_channels + i][0] = value[0];
                        joints_data[index * 2 * joint_channels + joint_channels + i][1] = value[1];
                        joints_data[index * 2 * joint_channels + joint_channels + i][2] = value[2];
                        joints_data[index * 2 * joint_channels + joint_channels + i][3] = value[3];
                    });
                }
                ot->joints = oscene.push_buffer(std::move(joints_data));
            }

            ot->primitive_topology = static_cast<uint8_t>(it->type);
            if (it->materialIndex)
                ot->material = it->materialIndex.value();
            else
                ot->material = 0;

            std::vector<fvec4> position_displacements(positions.size() * ht->weights.size()), normal_displacements(normals.size() * ht->weights.size()),
                ptp_pos(position_displacements.size()), ptp_nor(normal_displacements.size());
            ot->displace_weights.assign(ht->weights.begin(), ht->weights.end());
            for (size_t w = 0; w < ht->weights.size(); w++) {
                std::span<fvec4> positions_data = std::span(ptp_pos).subspan(w * positions.size()), normals_data = std::span(ptp_nor).subspan(w * normals.size());
                load_mesh_buffers(asset, positions_data, normals_data, uv_channels, color_channels, [w, it](std::string_view name) {
                    const fastgltf::Attribute* attr = it->findTargetAttribute(w, name);
                    return attr == it->targets[w].end() ? nullptr : attr;
                });
            }
            for (size_t w = 0; w < ht->weights.size(); w++) {
                for (size_t v = 0; v < ot->vertex_count; v++) {
                    memcpy(position_displacements[posn_bsize * (v * ht->weights.size() + w)].data(),
                        ptp_pos[posn_bsize * (v + ot->vertex_count * w)].data(),
                        posn_bsize * sizeof(fvec4));
                    memcpy(normal_displacements[norm_bsize * (v * ht->weights.size() + w)].data(),
                        ptp_nor[norm_bsize * (v + ot->vertex_count * w)].data(),
                        norm_bsize * sizeof(fvec4));
                }
            }

            ot->positions = oscene.push_buffer(positions);
            ot->normals = oscene.push_buffer(normals);
            ot->position_displacements = oscene.push_buffer(position_displacements);
            ot->normal_displacements = oscene.push_buffer(normal_displacements);
        }

        if (ht->name.size() > 0) {
            oscene.mesh_names[std::string(ht->name)] = mesh_groups.back();
        }
    }

    return mesh_groups;
}

static std::vector<size_t> load_skins(OutputScene& oscene, const fastgltf::Asset& asset)
{
    // skeletons: two "skins" are the same skeleton if they have equivalent joints.size and inverseBindMatrices accessor
    std::vector<std::pair<size_t, size_t>> skin_key(asset.skins.size());
    std::transform(asset.skins.cbegin(), asset.skins.cend(), skin_key.begin(), [](const fastgltf::Skin& skin) {
        return std::pair { skin.joints.size(), skin.inverseBindMatrices.value_or(std::numeric_limits<size_t>::max()) };
    });

    std::map<size_t, size_t> node_parents;
    for (size_t i = 0; i < asset.nodes.size(); i++) {
        for (size_t child : asset.nodes[i].children)
            node_parents[child] = i;
    }

    std::map<std::pair<size_t, size_t>, size_t> skin_dedup;
    for (const auto& k : skin_key) {
        auto [dedup_it, inserted] = skin_dedup.insert(std::pair { k, skin_dedup.size() });
        if (inserted) {
            const fastgltf::Skin& asset_skin = asset.skins[dedup_it->second];
            std::unique_ptr<fbs::SkeletonT>& skeleton = oscene->skeletons.emplace_back(std::make_unique<fbs::SkeletonT>());
            std::vector<fmat4x4> skin_matrices(k.first);
            if (k.second == std::numeric_limits<size_t>::max()) {
                fmat4x4 identity;
                for (size_t i = 0; i < k.first; i++)
                    memcpy(skin_matrices[i].data(), identity.data(), sizeof(fmat4x4));
            } else {
                fastgltf::copyFromAccessor<fmat4x4>(asset, asset.accessors[k.second], skin_matrices.data());
            }
            skeleton->skin_matrices = oscene.push_buffer(skin_matrices);

            // The bone nodes are all the children of the common ancestor of all the joints since the full graph is needed for rigging.
            // The joint nodes are the subset of the *bone nodes* that directly influence the skin.
            size_t skeleton_root = [&asset_skin, &node_parents]() {
                std::set<size_t> scene_bones(asset_skin.joints.begin(), asset_skin.joints.end());
                while (scene_bones.size() > 1) {
                    std::set<size_t> bone_parents;
                    for (size_t bone : scene_bones)
                        bone_parents.insert(node_parents[bone]);
                    scene_bones = bone_parents;
                }
                return *scene_bones.begin();
            }();
            skeleton->nodes.reserve(asset_skin.joints.size());
            skeleton->joints.reserve(asset_skin.joints.size());

            std::queue<size_t> nodes;
            std::vector<size_t> bone_to_node;
            std::map<size_t, size_t> node_to_bone;
            nodes.push(skeleton_root);
            while (nodes.empty() == false) {
                size_t node_index = nodes.front();
                skeleton->nodes.emplace_back(std::make_unique<fbs::BoneNodeT>());
                node_to_bone[node_index] = node_to_bone.size();
                bone_to_node.push_back(node_index);
                nodes.pop();

                skeleton->nodes.back()->children.resize(asset.nodes[node_index].children.size());
                for (size_t child : asset.nodes[node_index].children)
                    nodes.push(child);
                std::visit(fastgltf::visitor {
                               [&](const fastgltf::TRS& trs) {
                                   skeleton->nodes.back()->transform.Set(fbs::TRS(
                                       fbs::Vec4(std::span<const float, 4> { trs.rotation.data(), 4 }),
                                       fbs::Vec3(std::span<const float, 3> { trs.translation.data(), 3 }),
                                       fbs::Vec3(std::span<const float, 3> { trs.scale.data(), 3 })));
                               },
                               [&](const fmat4x4& mat) {
                                   auto columns = std::to_array({ fbs::Vec4(std::span<const float, 4> { mat.col(0).data(), 4 }),
                                       fbs::Vec4(std::span<const float, 4> { mat.col(1).data(), 4 }),
                                       fbs::Vec4(std::span<const float, 4> { mat.col(2).data(), 4 }),
                                       fbs::Vec4(std::span<const float, 4> { mat.col(3).data(), 4 }) });
                                   skeleton->nodes.back()->transform.Set(fbs::Mat4(columns));
                               },
                           },
                    asset.nodes[node_index].transform);
            }
            auto node_to_bone_fn = [&](size_t joint_node) {
                return node_to_bone[joint_node];
            };
            for (size_t i = 0; i < skeleton->nodes.size(); i++) {
                std::transform(asset.nodes[bone_to_node[i]].children.begin(),
                    asset.nodes[bone_to_node[i]].children.end(),
                    skeleton->nodes[i]->children.begin(), node_to_bone_fn);
            }
            std::transform(asset_skin.joints.begin(), asset_skin.joints.end(), std::back_inserter(skeleton->joints), node_to_bone_fn);
        }
    }

    std::vector<size_t> out_dedup(asset.skins.size());
    for (size_t i = 0; i < asset.skins.size(); i++)
        out_dedup[i] = skin_dedup[skin_key[i]];
    return out_dedup;
}

struct AnimationTargetT : public fbs::AnimationTargetT {
    size_t sampler, bucket, channel;
    bool operator<(const AnimationTargetT& other) const
    {
        if (sampler != other.sampler)
            return sampler < other.sampler;
        if (bucket != other.bucket)
            return bucket < other.bucket;
        return channel < other.channel;
    }
};

static void load_animations(OutputScene& oscene, const fastgltf::Asset& asset, std::span<size_t> skin_to_skeleton)
{
    oscene->animations.reserve(asset.animations.size());
    for (auto anim = asset.animations.begin(); anim != asset.animations.end(); ++anim) {
        std::map<size_t, std::array<std::vector<size_t>, 3>> sampler_outputs;
        std::vector<std::bitset<static_cast<int>(fbs::AnimationTargetField::MAX) + 1>> sampler_paths(anim->samplers.size());
        for (auto it = anim->channels.begin(); it != anim->channels.end(); ++it)
            sampler_paths[it->samplerIndex].set(static_cast<int>(it->path));
        for (auto it = anim->samplers.begin(); it != anim->samplers.end(); ++it) {
            assert(it->interpolation != fastgltf::AnimationInterpolation::CubicSpline);
            int sampler_index = std::distance(anim->samplers.begin(), it), bucket;
            if (it->interpolation == fastgltf::AnimationInterpolation::Step)
                sampler_outputs[it->inputAccessor][0].push_back(it->outputAccessor);
            else {
                auto paths = sampler_paths[sampler_index], paths_xr = paths;
                if (paths_xr.reset(static_cast<int>(fbs::AnimationTargetField::Rotation)).any())
                    sampler_outputs[it->inputAccessor][1].push_back(it->outputAccessor);
                if (paths.test(static_cast<int>(fbs::AnimationTargetField::Rotation)))
                    sampler_outputs[it->inputAccessor][2].push_back(it->outputAccessor);
            }
        }
        for (auto it = sampler_outputs.begin(); it != sampler_outputs.end(); ++it) {
            for (auto jt = it->second.begin(); jt != it->second.end(); ++jt)
                std::sort(jt->begin(), jt->end());
        }

        std::vector<AnimationTargetT> targets;
        targets.reserve(anim->channels.size());
        for (auto it = anim->channels.begin(); it != anim->channels.end(); ++it) {
            if (it->nodeIndex) {
                AnimationTargetT& target = targets.emplace_back();
                size_t sampler_key = anim->samplers[it->samplerIndex].inputAccessor, channel_bucket = 1;
                if (anim->samplers[it->samplerIndex].interpolation == fastgltf::AnimationInterpolation::Step)
                    channel_bucket = 0;
                else if (it->path == fastgltf::AnimationPath::Rotation)
                    channel_bucket = 2;
                auto channel_it = std::find(sampler_outputs[sampler_key][channel_bucket].begin(),
                    sampler_outputs[sampler_key][channel_bucket].end(),
                    anim->samplers[it->samplerIndex].outputAccessor);
                assert(channel_it != sampler_outputs[sampler_key][channel_bucket].end());
                target.object = it->nodeIndex.value();
                target.width = 1;
                target.field = static_cast<fbs::AnimationTargetField>(it->path);
                target.sampler = sampler_key;
                target.bucket = channel_bucket;
                target.channel = std::distance(sampler_outputs[sampler_key][channel_bucket].begin(), channel_it);

                if (it->path == fastgltf::AnimationPath::Weights) {
                    const fastgltf::Accessor& output_data = asset.accessors[anim->samplers[it->samplerIndex].outputAccessor];
                    size_t unpadded_size = fastgltf::getNumComponents(output_data.type) * output_data.count,
                           unpadded_width = unpadded_size / asset.accessors[anim->samplers[it->samplerIndex].inputAccessor].count;
                    target.width = ((unpadded_width + 3) >> 2);
                } else {
                    // If this node is part of a skeleton, it might get pruned.
                    for (size_t i = 0; i < asset.skins.size(); i++) {
                        auto it = std::find(asset.skins[i].joints.begin(), asset.skins[i].joints.end(), target.object);
                        if (it != asset.skins[i].joints.end()) {
                            target.object = std::distance(asset.skins[i].joints.begin(), it);
                            target.object_is_bone = true;
                        }
                    }
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        if (anim->name.size() > 0)
            oscene.animation_names[std::string(anim->name)] = oscene->animations.size();

        auto& out_anim = oscene->animations.emplace_back(std::make_unique<fbs::AnimationT>());
        for (auto it = sampler_outputs.begin(); it != sampler_outputs.end(); ++it) {
            auto& sampler = out_anim->samplers.emplace_back(std::make_unique<fbs::AnimationSamplerT>());
            std::vector<float> timeline(asset.accessors[it->first].count), channels, channel_data;
            channels.resize(std::transform_reduce(it->second.begin(), it->second.end(), 0, std::plus<size_t>(), [&](const std::vector<size_t>& v) {
                return std::transform_reduce(v.begin(), v.end(), 0, std::plus<size_t>(), [&](size_t ai) {
                    const fastgltf::Accessor& acc = asset.accessors[ai];
                    size_t unpadded_size = fastgltf::getNumComponents(acc.type) * acc.count,
                           unpadded_width = unpadded_size / timeline.size(),
                           padded_width = (unpadded_width + 3) & (~3);
                    return padded_width * timeline.size();
                });
            }));

            std::array<uint32_t, 3> target_count {};
            size_t keyframe_width = channels.size() / timeline.size(), kf_offset = 0;
            fastgltf::copyFromAccessor<float>(asset, asset.accessors[it->first], timeline.data());
            for (auto jt = it->second.begin(); jt != it->second.end(); ++jt) {
                for (auto at = jt->begin(); at != jt->end(); ++at) {
                    const fastgltf::Accessor& acc = asset.accessors[*at];
                    size_t unpadded_size = fastgltf::getNumComponents(acc.type) * acc.count,
                           unpadded_width = unpadded_size / timeline.size(),
                           padded_width = (unpadded_width + 3) & (~3);
                    channel_data.resize(unpadded_size);
                    switch (acc.type) {
                    case fastgltf::AccessorType::Scalar:
                        fastgltf::copyFromAccessor<float>(asset, acc, channel_data.data());
                        break;
                    case fastgltf::AccessorType::Vec3:
                        fastgltf::copyFromAccessor<fvec3>(asset, acc, reinterpret_cast<fvec3*>(channel_data.data()));
                        break;
                    case fastgltf::AccessorType::Vec4:
                        fastgltf::copyFromAccessor<fvec4>(asset, acc, reinterpret_cast<fvec4*>(channel_data.data()));
                        break;
                    default:
                        std::unreachable();
                    }

                    for (size_t i = 0; i < timeline.size(); i++) {
                        std::copy_n(channel_data.begin() + unpadded_width * i, unpadded_width, channels.begin() + keyframe_width * i + kf_offset);
                        std::fill_n(channels.begin() + keyframe_width * i + kf_offset + unpadded_width, padded_width - unpadded_width, 0.f);
                    }
                    kf_offset += padded_width;
                    target_count[jt - it->second.begin()] += padded_width;
                }
            }

            sampler->timeline = oscene.push_buffer(timeline);
            sampler->channels = oscene.push_buffer(channels);
            sampler->step_targets = target_count[0] >> 2;
            sampler->lerp_targets = target_count[1] >> 2;
            sampler->slerp_targets = target_count[2] >> 2;
        }
        std::transform(targets.begin(), targets.end(), std::back_inserter(out_anim->targets),
            [](const AnimationTargetT& target) { return std::make_unique<fbs::AnimationTargetT>(target); });
    }
}

static void load_scene(OutputScene& oscene, const fastgltf::Asset& asset, const std::vector<std::vector<uint32_t>>& mesh_groups, std::span<size_t> skin_to_skeleton)
{
    std::set<uint32_t> prune_set, unprune_set;
    std::stack<std::pair<uint32_t, bool>> prune_stack;
    prune_stack.emplace(0, false);
    while (prune_stack.empty() == false) {
        auto [node, visited] = prune_stack.top();
        prune_stack.pop();

        if (visited) {
            if (asset.nodes[node].cameraIndex.has_value() == false && asset.nodes[node].meshIndex.has_value() == false) {
                std::set<uint32_t> node_children(asset.nodes[node].children.begin(), asset.nodes[node].children.end());
                if (std::includes(prune_set.begin(), prune_set.end(), node_children.begin(), node_children.end())) {
                    prune_set.insert(node);
                }
            }
        } else {
            prune_stack.emplace(node, true);
            for (auto it = asset.nodes[node].children.begin(); it != asset.nodes[node].children.end(); ++it) {
                if (prune_set.contains(*it) == false)
                    prune_stack.emplace(*it, false);
            }
        }
    }
    for (auto it = asset.skins.begin(); it != asset.skins.end(); ++it) {
        // If any nodes in that skin are not to be pruned, we cannot prune any of them. This keeps logic simple in the rendering engine.
        if (!std::all_of(it->joints.begin(), it->joints.end(), [&prune_set](size_t j) { return prune_set.contains(j); })) {
            for (auto jt = it->joints.begin(); jt != it->joints.end(); ++jt)
                unprune_set.insert(*jt);
        }
    }
    std::erase_if(prune_set, [&unprune_set](uint32_t n) { return unprune_set.contains(n); });

    std::vector<uint32_t> ossified_prune_set(prune_set.begin(), prune_set.end());
    auto translate_node_index = [prune_set = std::move(ossified_prune_set)](uint32_t in_index) -> uint32_t {
        auto it = std::lower_bound(prune_set.begin(), prune_set.end(), in_index);
        if (it != prune_set.end() && *it == in_index)
            return std::numeric_limits<uint32_t>::max();
        else
            return in_index - static_cast<uint32_t>(it - prune_set.begin());
    };

    std::generate_n(std::back_inserter(oscene->nodes), asset.nodes.size() - prune_set.size(), []() { return std::make_unique<fbs::SceneNodeT>(); });
    for (size_t i = 0; i < asset.nodes.size(); i++) {
        if (prune_set.contains(i))
            continue;
        if (asset.nodes[i].name.size() > 0)
            oscene.node_names[std::string(asset.nodes[i].name)] = translate_node_index(i);

        std::unique_ptr<fbs::SceneNodeT>& out_node = oscene->nodes[translate_node_index(i)];
        if (asset.nodes[i].cameraIndex)
            out_node->camera = asset.nodes[i].cameraIndex.value() + 1;
        for (auto it = asset.nodes[i].children.begin(); it != asset.nodes[i].children.end(); ++it) {
            uint32_t out_index = translate_node_index(*it);
            if (out_index != std::numeric_limits<uint32_t>::max())
                out_node->children.push_back(out_index);
        }
        if (asset.nodes[i].meshIndex)
            out_node->mesh = mesh_groups[asset.nodes[i].meshIndex.value()];
        if (asset.nodes[i].skinIndex) {
            std::vector<uint32_t> skin;
            auto& jni = asset.skins[asset.nodes[i].skinIndex.value()].joints;
            std::transform(jni.begin(), jni.end(), std::back_inserter(skin), translate_node_index);
            out_node->skeleton = skin_to_skeleton[asset.nodes[i].skinIndex.value()];
            if (std::any_of(skin.begin(), skin.end(), [](uint32_t i) { return i != std::numeric_limits<uint32_t>::max(); }))
                out_node->skin = std::move(skin);
        }
        std::copy(asset.nodes[i].weights.begin(), asset.nodes[i].weights.end(), std::back_inserter(out_node->displace_weights));
        std::visit(fastgltf::visitor {
                       [&](const fastgltf::TRS& trs) {
                           out_node->transform.Set(fbs::TRS(
                               fbs::Vec4(std::span<const float, 4> { trs.rotation.data(), 4 }),
                               fbs::Vec3(std::span<const float, 3> { trs.translation.data(), 3 }),
                               fbs::Vec3(std::span<const float, 3> { trs.scale.data(), 3 })));
                       },
                       [&](const fmat4x4& mat) {
                           auto columns = std::to_array({ fbs::Vec4(std::span<const float, 4> { mat.col(0).data(), 4 }),
                               fbs::Vec4(std::span<const float, 4> { mat.col(1).data(), 4 }),
                               fbs::Vec4(std::span<const float, 4> { mat.col(2).data(), 4 }),
                               fbs::Vec4(std::span<const float, 4> { mat.col(3).data(), 4 }) });
                           out_node->transform.Set(fbs::Mat4(columns));
                       },
                   },
            asset.nodes[i].transform);
    }

    auto& root_nodes = asset.scenes[asset.defaultScene.value_or(0)].nodeIndices;
    for (auto it = root_nodes.begin(); it != root_nodes.end(); ++it) {
        uint32_t out_index = translate_node_index(*it);
        if (out_index != std::numeric_limits<uint32_t>::max())
            oscene->roots.push_back(out_index);
    }
}

static void load_images(OutputScene& oscene, const fastgltf::Asset& asset)
{
    std::set<uint32_t> vector_images;
    for (auto it = asset.materials.begin(); it != asset.materials.end(); ++it) {
        if (it->anisotropy && it->anisotropy->anisotropyTexture && asset.textures[it->anisotropy->anisotropyTexture->textureIndex].imageIndex.has_value())
            vector_images.insert(asset.textures[it->anisotropy->anisotropyTexture->textureIndex].imageIndex.value());
        if (it->normalTexture && asset.textures[it->normalTexture->textureIndex].imageIndex.has_value())
            vector_images.insert(asset.textures[it->normalTexture->textureIndex].imageIndex.value());
    }

    oscene->images.reserve(asset.images.size());
    for (size_t i = 0; i < asset.images.size(); i++) {
        bool is_vector_image = vector_images.contains(i);
        size_t index = std::visit(fastgltf::visitor {
                                      [&](auto&& container) -> size_t {
                                          if constexpr (std::is_same<std::decay_t<decltype(container)>, fastgltf::sources::BufferView>::value) {
                                              const fastgltf::BufferView& buffer_view = asset.bufferViews[container.bufferViewIndex];
                                              const fastgltf::Buffer& buffer = asset.buffers[buffer_view.bufferIndex];
                                              return std::visit(fastgltf::visitor {
                                                                    [&](auto& container) -> size_t {
                                                                        if constexpr (requires { container.bytes; }) {
                                                                            std::span<const std::byte> container_span = container.bytes,
                                                                                                       image_span = container_span.subspan(buffer_view.byteOffset, buffer_view.byteLength);
                                                                            return oscene.push_buffer(generate_image(image_span, is_vector_image));
                                                                        } else {
                                                                            abort();
                                                                        }
                                                                    },
                                                                },
                                                  buffer.data);
                                          } else if constexpr (requires { container.bytes; }) {
                                              return oscene.push_buffer(generate_image(container.bytes, is_vector_image));
                                          } else {
                                              abort();
                                          }
                                      } },
            asset.images[i].data);
        if (asset.images[i].name.size() > 0)
            oscene.image_names[std::string(asset.images[i].name)] = oscene->images.size();
        if (index != 0)
            oscene->images.push_back(index);
    }
}

void load_gltf(OutputScene& oscene, const fastgltf::Asset& asset)
{
    load_materials(oscene, asset.materials, asset.textures);
    std::cerr << "[I] Loaded " << oscene->materials.size() << " materials" << std::endl;

    std::vector<std::vector<uint32_t>> mesh_groups = load_meshes(oscene, asset, max_uv_channels, max_color_channels, max_joint_channels);
    std::cerr << "[I] Extracted " << mesh_groups.size() << " new -> " << oscene->meshes.size() << " meshes" << std::endl;

    std::vector<size_t> skin_to_skeleton = load_skins(oscene, asset);
    std::cerr << "[I] Loaded " << skin_to_skeleton.size() << " new skins to " << oscene->skeletons.size() << " skeletons" << std::endl;

    load_animations(oscene, asset, skin_to_skeleton);
    std::cerr << "[I] Extracted " << oscene->animations.size() << " animations" << std::endl;

    load_images(oscene, asset);
    std::cerr << "[I] Extracted " << oscene->images.size() << " images" << std::endl;

    load_scene(oscene, asset, mesh_groups, skin_to_skeleton);
    std::cerr << "[I] Loaded " << oscene->nodes.size() << " scene items" << std::endl;
}
