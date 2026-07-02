#include "gltf2tg.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <stack>
#include <vector>
#include <cglm/struct.h>
#include <fastgltf/core.hpp>
#include "asset.fbs.hpp"

namespace fs = std::filesystem;
namespace fbs = twogame::fbs;

int usage(const char* argv0, const std::string& extra = "")
{
    std::cerr << "usage: " << argv0 << " [-s] -o OUTFILE INFILE" << std::endl
              << "\t-s: enable UASTC compression of image mip levels" << std::endl
              << "\tINFILE must be a valid glTF file." << std::endl;
    if (extra.empty() == false)
        std::cerr << std::endl
                  << '\t' << extra << std::endl;
    return 1;
}

class BufferTrain {
    struct IBuffer {
        virtual ~IBuffer() = default;
        virtual std::span<const std::byte> bytes() const = 0;
        size_t byte_size() const { return bytes().size_bytes(); }
    };
    template <typename T>
    struct TypedBuffer : public IBuffer {
        std::vector<T> m_vec;
        explicit TypedBuffer(std::vector<T>&& vec)
            : m_vec(std::move(vec))
        {
        }
        std::span<const std::byte> bytes() const
        {
            return std::as_bytes(std::span(m_vec));
        }
    };
    struct ImageBuffer : public IBuffer {
        ImageGenerator::SerializedImage m_image;
        explicit ImageBuffer(ImageGenerator::SerializedImage&& image)
            : m_image(std::move(image))
        {
        }
        std::span<const std::byte> bytes() const
        {
            return m_image.as_bytes();
        }
    };
    std::vector<std::unique_ptr<IBuffer>> m_train;

public:
    size_t count() const { return m_train.size(); }
    size_t size(size_t index) const { return m_train.at(index)->byte_size(); }

    template <typename T>
    size_t push(std::vector<T>&& vec)
    {
        if (vec.empty())
            return 0;
        m_train.push_back(std::make_unique<TypedBuffer<T>>(std::move(vec)));
        return m_train.size();
    }
    size_t push(ImageGenerator::SerializedImage&& image)
    {
        if (image.as_bytes().empty())
            return 0;
        m_train.push_back(std::make_unique<ImageBuffer>(std::move(image)));
        return m_train.size();
    }
    void dump(const std::string& asset_path)
    {
        size_t ext_offset = asset_path.find_last_of('.');
        std::vector<char> slice_name(ext_offset + 16);
        std::copy(asset_path.begin(), asset_path.begin() + ext_offset, slice_name.begin());
        for (size_t i = 0; i < m_train.size(); i++) {
            snprintf(slice_name.data() + ext_offset, 16, ".%zu.bin", i + 1);
            std::ofstream slice_stream(slice_name.data(), std::ios::binary | std::ios::out);
            std::span<const std::byte> slice = m_train[i]->bytes();
            slice_stream.write(reinterpret_cast<const char*>(slice.data()), slice.size_bytes());
            slice_stream.close();
        }
    }
};

void load_materials(fbs::AssetsT& out_assets, const std::vector<fastgltf::Material>& materials, const std::vector<fastgltf::Texture>& textures)
{
    for (auto it = materials.begin(); it != materials.end(); ++it) {
        auto& ot = out_assets.materials.emplace_back(std::make_unique<fbs::MaterialT>());
        ot->base_color_factor = std::make_unique<fbs::Vec4>(std::span<const float, 4> { it->pbrData.baseColorFactor.data(), 4 });
        if (it->pbrData.baseColorTexture) {
            auto& texture = textures[it->pbrData.baseColorTexture.value().textureIndex];
            ot->base_color_texture = std::make_unique<fbs::TextureT>();
            ot->base_color_texture->image = texture.imageIndex.value_or(0);
            ot->base_color_texture->sampler = texture.samplerIndex.value_or(0);
            ot->base_color_uv = it->pbrData.baseColorTexture.value().texCoordIndex;
        }
        ot->double_sided = it->doubleSided;
        ot->emissive_factor = std::make_unique<fbs::Vec3>(std::span<const float, 3> { it->emissiveFactor.data(), 3 });
        if (it->emissiveTexture) {
            auto& texture = textures[it->emissiveTexture.value().textureIndex];
            ot->emissive_texture = std::make_unique<fbs::TextureT>();
            ot->emissive_texture->image = texture.imageIndex.value_or(0);
            ot->emissive_texture->sampler = texture.samplerIndex.value_or(0);
            ot->emissive_uv = it->emissiveTexture.value().texCoordIndex;
        }
        ot->metallic_factor = it->pbrData.metallicFactor;
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
        ot->roughness_factor = it->pbrData.roughnessFactor;
        ot->unlit = it->unlit;
    }
}

template <typename FindAttribute>
    requires std::invocable<FindAttribute, std::string_view> && std::same_as<std::invoke_result_t<FindAttribute, std::string_view>, const fastgltf::Attribute*>
void load_mesh_buffers(const fastgltf::Asset& asset, std::span<vec4s> position, std::span<vec4s> normal, size_t uv_channels, size_t color_channels, FindAttribute&& find_attribute)
{
    std::span<vec2s> uv(reinterpret_cast<vec2s*>(position.data()), position.size() * 2);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, asset.accessors[find_attribute("POSITION")->accessorIndex], [&](fastgltf::math::fvec3 value, size_t index) {
        position[index * ((uv_channels + 3) / 2)].x = value[0];
        position[index * ((uv_channels + 3) / 2)].y = value[1];
        position[index * ((uv_channels + 3) / 2)].z = value[2];
        position[index * ((uv_channels + 3) / 2)].w = 1.f;
    });

    const fastgltf::Attribute* attribute;
    char accessor_name[16];
    for (size_t i = 0; i < uv_channels; i++) {
        snprintf(accessor_name, 16, "TEXCOORD_%zu", i);
        if ((attribute = find_attribute(accessor_name)) == nullptr)
            break;

        auto& accessor = asset.accessors[attribute->accessorIndex];
        if (accessor.componentType == fastgltf::ComponentType::Byte) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::s8vec2>(asset, accessor, [&](fastgltf::math::s8vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].u = std::clamp(value[0] / 127.f, -1.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].v = std::clamp(value[1] / 127.f, -1.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::u8vec2>(asset, accessor, [&](fastgltf::math::u8vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].u = std::clamp(value[0] / 255.f, 0.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].v = std::clamp(value[1] / 255.f, 0.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::Short) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::s16vec2>(asset, accessor, [&](fastgltf::math::s16vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].u = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].v = std::clamp(value[1] / 32767.f, -1.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec2>(asset, accessor, [&](fastgltf::math::u16vec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].u = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].v = std::clamp(value[1] / 65535.f, 0.f, 1.f);
            });
        } else if (accessor.componentType == fastgltf::ComponentType::Float) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, accessor, [&](fastgltf::math::fvec2 value, size_t index) {
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].u = value[0];
                uv[index * ((uv_channels + 3) & (~1)) + 2 + i].v = value[1];
            });
        } else {
            assert(false);
        }
    }

    if ((attribute = find_attribute("NORMAL")) != nullptr) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, asset.accessors[attribute->accessorIndex], [&](fastgltf::math::fvec3 value, size_t index) {
            normal[index * (2 + color_channels) + 0].x = value[0];
            normal[index * (2 + color_channels) + 0].y = value[1];
            normal[index * (2 + color_channels) + 0].z = value[2];
            normal[index * (2 + color_channels) + 0].w = 0.f;
        });
    }
    if ((attribute = find_attribute("TANGENT")) != nullptr) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, asset.accessors[attribute->accessorIndex], [&](fastgltf::math::fvec4 value, size_t index) {
            normal[index * (2 + color_channels) + 1].x = value[0];
            normal[index * (2 + color_channels) + 1].y = value[1];
            normal[index * (2 + color_channels) + 1].z = value[2];
            normal[index * (2 + color_channels) + 1].w = value[3];
        });
    }
    for (size_t i = 0; i < color_channels; i++) {
        snprintf(accessor_name, 16, "COLOR_%zu", i);
        if ((attribute = find_attribute(accessor_name)) == nullptr)
            break;

        auto& accessor = asset.accessors[attribute->accessorIndex];
        if (accessor.type == fastgltf::AccessorType::Vec3) {
            if (accessor.componentType == fastgltf::ComponentType::Byte) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::s8vec3>(asset, accessor, [&](fastgltf::math::s8vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::u8vec3>(asset, accessor, [&](fastgltf::math::u8vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Short) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::s16vec3>(asset, accessor, [&](fastgltf::math::s16vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec3>(asset, accessor, [&](fastgltf::math::u16vec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = 0;
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Float) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, accessor, [&](fastgltf::math::fvec3 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = value[0];
                    normal[index * (2 + color_channels) + 2 + i].g = value[1];
                    normal[index * (2 + color_channels) + 2 + i].b = value[2];
                    normal[index * (2 + color_channels) + 2 + i].a = 0;
                });
            } else {
                assert(false);
            }
        } else if (accessor.type == fastgltf::AccessorType::Vec4) {
            if (accessor.componentType == fastgltf::ComponentType::Byte) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::s8vec4>(asset, accessor, [&](fastgltf::math::s8vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 127.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = std::clamp(value[3] / 127.f, -1.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::u8vec4>(asset, accessor, [&](fastgltf::math::u8vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 255.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = std::clamp(value[3] / 255.f, 0.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Short) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::s16vec4>(asset, accessor, [&](fastgltf::math::s16vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 32767.f, -1.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = std::clamp(value[3] / 32767.f, -1.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(asset, accessor, [&](fastgltf::math::u16vec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = std::clamp(value[0] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].g = std::clamp(value[1] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].b = std::clamp(value[2] / 65535.f, 0.f, 1.f);
                    normal[index * (2 + color_channels) + 2 + i].a = std::clamp(value[3] / 65535.f, 0.f, 1.f);
                });
            } else if (accessor.componentType == fastgltf::ComponentType::Float) {
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, accessor, [&](fastgltf::math::fvec4 value, size_t index) {
                    normal[index * (2 + color_channels) + 2 + i].r = value[0];
                    normal[index * (2 + color_channels) + 2 + i].g = value[1];
                    normal[index * (2 + color_channels) + 2 + i].b = value[2];
                    normal[index * (2 + color_channels) + 2 + i].a = value[3];
                });
            } else {
                assert(false);
            }
        } else {
            assert(false);
        }
    }
}

std::vector<std::vector<uint32_t>> load_meshes(fbs::AssetsT& out_assets, BufferTrain& out_data, const fastgltf::Asset& asset, size_t uv_channels, size_t color_channels, size_t joint_channels)
{
    std::vector<std::vector<uint32_t>> mesh_groups;
    for (auto ht = asset.meshes.begin(); ht != asset.meshes.end(); ++ht) {
        mesh_groups.emplace_back();
        for (auto it = ht->primitives.begin(); it != ht->primitives.end(); ++it) {
            mesh_groups.back().push_back(out_assets.meshes.size());
            auto& ot = out_assets.meshes.emplace_back(std::make_unique<fbs::MeshT>());

            auto& indexes_accessor = asset.accessors[it->indicesAccessor.value()];
            ot->index_count = indexes_accessor.count;
            ot->vertex_count = asset.accessors[it->findAttribute("POSITION")->accessorIndex].count;
            if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
                std::vector<uint16_t> indexes(ot->index_count);
                fastgltf::iterateAccessorWithIndex<uint8_t>(asset, indexes_accessor, [&](uint8_t value, size_t index) {
                    indexes[index] = value;
                });
                ot->indexes = out_data.push(std::move(indexes));
            } else if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
                std::vector<uint16_t> indexes(ot->index_count);
                fastgltf::copyFromAccessor<uint16_t>(asset, indexes_accessor, indexes.data());
                ot->indexes = out_data.push(std::move(indexes));
            } else if (indexes_accessor.componentType == fastgltf::ComponentType::UnsignedInt) {
                std::vector<uint32_t> indexes(ot->index_count);
                fastgltf::copyFromAccessor<uint32_t>(asset, indexes_accessor, indexes.data());
                ot->indexes = out_data.push(std::move(indexes));
            } else {
                assert(false);
            }

            std::vector<vec4s> positions(ot->vertex_count * ((uv_channels + 3) / 2)), normals(ot->vertex_count * (2 + color_channels));
            load_mesh_buffers(asset, positions, normals, uv_channels, color_channels, [it](std::string_view name) {
                const fastgltf::Attribute* attr = it->findAttribute(name);
                return attr == it->attributes.end() ? nullptr : attr;
            });

            if (it->findAttribute("JOINTS_0") != it->attributes.end()) {
                std::vector<vec4s> joints_data(ot->vertex_count * joint_channels * 2);
                const fastgltf::Attribute* attribute;
                char accessor_name[16];
                ivec4s* joints_index_data = reinterpret_cast<ivec4s*>(joints_data.data());
                for (size_t i = 0; i < joint_channels; i++) {
                    snprintf(accessor_name, 16, "JOINTS_%zu", i);
                    if ((attribute = it->findAttribute(accessor_name)) == it->attributes.end())
                        break;

                    fastgltf::iterateAccessorWithIndex<fastgltf::math::ivec4>(asset, asset.accessors[attribute->accessorIndex], [&](fastgltf::math::ivec4 value, size_t index) {
                        joints_index_data[index * 2 * joint_channels + i].x = value[0];
                        joints_index_data[index * 2 * joint_channels + i].y = value[1];
                        joints_index_data[index * 2 * joint_channels + i].z = value[2];
                        joints_index_data[index * 2 * joint_channels + i].w = value[3];
                    });

                    snprintf(accessor_name, 16, "WEIGHTS_%zu", i);
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, asset.accessors[it->findAttribute(accessor_name)->accessorIndex], [&](fastgltf::math::fvec4 value, size_t index) {
                        joints_data[index * 2 * joint_channels + joint_channels + i].x = value[0];
                        joints_data[index * 2 * joint_channels + joint_channels + i].y = value[1];
                        joints_data[index * 2 * joint_channels + joint_channels + i].z = value[2];
                        joints_data[index * 2 * joint_channels + joint_channels + i].w = value[3];
                    });
                }
                ot->joints = out_data.push(std::move(joints_data));
            }

            ot->primitive_topology = static_cast<uint8_t>(it->type);
            if (it->materialIndex)
                ot->material = it->materialIndex.value();
            else
                ot->material = std::nullopt;

            std::vector<vec4s> position_displacements(positions.size() * ht->weights.size()), normal_displacements(normals.size() * ht->weights.size());
            ot->displace_weights.assign(ht->weights.begin(), ht->weights.end());
            for (size_t i = 0; i < ht->weights.size(); i++) {
                std::span<vec4s> positions_data = std::span(position_displacements).subspan(i * positions.size()), normals_data = std::span(normal_displacements).subspan(i * normals.size());
                load_mesh_buffers(asset, positions_data, normals_data, uv_channels, color_channels, [i, it](std::string_view name) {
                    const fastgltf::Attribute* attr = it->findTargetAttribute(i, name);
                    return attr == it->targets[i].end() ? nullptr : attr;
                });
            }

            ot->positions = out_data.push(std::move(positions));
            ot->normals = out_data.push(std::move(normals));
        }
    }

    return mesh_groups;
}

std::vector<size_t> load_skins(fbs::AssetsT& out_assets, BufferTrain& out_data, const fastgltf::Asset& asset)
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
            std::unique_ptr<fbs::SkeletonT>& skeleton = out_assets.skeletons.emplace_back(std::make_unique<fbs::SkeletonT>());
            std::vector<fastgltf::math::fmat4x4> skin_matrices(k.first);
            if (k.second == std::numeric_limits<size_t>::max()) {
                mat4 identity = GLM_MAT4_IDENTITY_INIT;
                for (size_t i = 0; i < k.first; i++)
                    memcpy(skin_matrices[i].data(), identity, sizeof(mat4));
            } else {
                fastgltf::copyFromAccessor<fastgltf::math::fmat4x4>(asset, asset.accessors[k.second], skin_matrices.data());
            }
            skeleton->skin_matrices = out_data.push(std::move(skin_matrices));

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
                               [&](const fastgltf::math::fmat4x4& mat) {
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
    size_t sampler;
    size_t channel;
    bool operator<(const AnimationTargetT& other) const
    {
        if (sampler != other.sampler)
            return sampler < other.sampler;
        return channel < other.channel;
    }
};

void load_animations(fbs::AssetsT& out_assets, BufferTrain& out_data, const fastgltf::Asset& asset, std::span<size_t> skin_to_skeleton)
{
    out_assets.animations.reserve(asset.animations.size());
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

        auto& out_anim = out_assets.animations.emplace_back(std::make_unique<fbs::AnimationT>());
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
                        fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset, acc, reinterpret_cast<fastgltf::math::fvec3*>(channel_data.data()));
                        break;
                    case fastgltf::AccessorType::Vec4:
                        fastgltf::copyFromAccessor<fastgltf::math::fvec4>(asset, acc, reinterpret_cast<fastgltf::math::fvec4*>(channel_data.data()));
                        break;
                    default:
                        assert(false);
                    }

                    for (size_t i = 0; i < timeline.size(); i++) {
                        std::copy_n(channel_data.begin() + unpadded_width * i, unpadded_width, channels.begin() + keyframe_width * i + kf_offset);
                        std::fill_n(channels.begin() + keyframe_width * i + kf_offset + unpadded_width, padded_width - unpadded_width, 0.f);
                    }
                    kf_offset += padded_width;
                    target_count[jt - it->second.begin()] += padded_width;
                }
            }

            sampler->timeline = out_data.push(std::move(timeline));
            sampler->channels = out_data.push(std::move(channels));
            sampler->step_targets = target_count[0] >> 2;
            sampler->lerp_targets = target_count[1] >> 2;
            sampler->slerp_targets = target_count[2] >> 2;
        }
        std::transform(targets.begin(), targets.end(), std::back_inserter(out_anim->targets),
            [](const AnimationTargetT& target) { return std::make_unique<fbs::AnimationTargetT>(target); });
    }
}

void load_scene(fbs::AssetsT& out_assets, const fastgltf::Asset& asset, const std::vector<std::vector<uint32_t>>& mesh_groups, std::span<size_t> skin_to_skeleton)
{
    std::set<uint32_t> prune_set, unprune_set;
    std::stack<std::pair<uint32_t, bool>> prune_stack;
    prune_stack.emplace(0, false);
    while (prune_stack.empty() == false) {
        auto [node, visited] = prune_stack.top();
        prune_stack.pop();

        if (visited) {
            if (asset.nodes[node].cameraIndex.has_value() == false && asset.nodes[node].meshIndex.has_value() == false && asset.nodes[node].lightIndex.has_value() == false) {
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

    std::generate_n(std::back_inserter(out_assets.nodes), asset.nodes.size() - prune_set.size(), []() { return std::make_unique<fbs::SceneNodeT>(); });
    for (size_t i = 0; i < asset.nodes.size(); i++) {
        if (prune_set.contains(i))
            continue;

        std::unique_ptr<fbs::SceneNodeT>& out_node = out_assets.nodes[translate_node_index(i)];
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
                       [&](const fastgltf::math::fmat4x4& mat) {
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
            out_assets.roots.push_back(out_index);
    }
}

void load_images(fbs::AssetsT& out_assets, BufferTrain& out_data, const fastgltf::Asset& asset, ImageGenerator& makeimage, const fs::path& out_name)
{
    out_assets.images.reserve(asset.images.size());
    for (size_t i = 0; i < asset.images.size(); i++) {
        size_t index = std::visit(fastgltf::visitor {
                                      [&](auto& container) -> size_t {
                                          if constexpr (requires { container.bytes; }) {
                                              return out_data.push(makeimage.generate(container.bytes));
                                          } else {
                                              return 0;
                                          }
                                      },
                                      [&](fastgltf::sources::BufferView& ref) -> size_t {
                                          const fastgltf::BufferView& buffer_view = asset.bufferViews[ref.bufferViewIndex];
                                          const fastgltf::Buffer& buffer = asset.buffers[buffer_view.bufferIndex];
                                          return std::visit(fastgltf::visitor {
                                                                [&](auto& container) -> size_t {
                                                                    if constexpr (requires { container.bytes; }) {
                                                                        std::span<const std::byte> container_span = container.bytes;
                                                                        return out_data.push(makeimage.generate(container_span.subspan(buffer_view.byteOffset, buffer_view.byteLength)));
                                                                    } else {
                                                                        return 0;
                                                                    }
                                                                },
                                                            },
                                              buffer.data);
                                      },
                                  },
            asset.images[i].data);
        if (index != 0)
            out_assets.images.push_back(index);
    }
}

void load_names(fbs::AssetsT& out_assets, const fastgltf::Asset& asset)
{
    out_assets.names = std::make_unique<fbs::NamesT>();
    for (auto it = asset.images.begin(); it != asset.images.end(); ++it)
        out_assets.names->images.emplace_back(it->name);
    for (auto it = asset.materials.begin(); it != asset.materials.end(); ++it)
        out_assets.names->materials.emplace_back(it->name);
    for (auto it = asset.meshes.begin(); it != asset.meshes.end(); ++it)
        out_assets.names->meshes.emplace_back(it->name);
    for (auto it = asset.animations.begin(); it != asset.animations.end(); ++it)
        out_assets.names->animations.emplace_back(it->name);
}

int main(int argc, char** argv)
{
    fs::path infile, outparam;
    fastgltf::Parser parser;
    bool enable_uastc = false;
    size_t max_uv_channels = 2, max_color_channels = 1, max_joint_channels = 1;

    for (int i = 0; ++i < argc;) {
        if (strcmp(argv[i], "-o") == 0) {
            outparam = argv[++i];
        } else if (strcmp(argv[i], "--max-uvs") == 0) {
            if ((max_uv_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-uvs: " << argv[i] << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--max-colors") == 0) {
            if ((max_color_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-colors: " << argv[i] << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--max-joints") == 0) {
            if ((max_joint_channels = atoi(argv[++i])) == 0) {
                std::cerr << "invalid value for --max-joints: " << argv[i] << std::endl;
                return 1;
            }
            max_joint_channels = (max_joint_channels + 3) & (~3);
        } else if (argv[i][0] == '-') {
            int j = 0;
            while (argv[i][++j]) {
                switch (argv[i][j]) {
                case 's':
                    enable_uastc = true;
                    break;
                case 0:
                    break;
                default:
                    std::cerr << "unknown option -" << argv[i][j] << std::endl;
                    return 1;
                }
            }
        } else {
            infile = argv[i];
        }
    }

    assert(volkInitialize() == VK_SUCCESS);
    if (infile.empty() || outparam.empty())
        return usage(*argv);

    outparam = fs::absolute(outparam);
    if (fs::is_directory(outparam.parent_path()) == false)
        return usage(*argv, "OUTFILE: file not found");
    if (fs::is_regular_file(infile) == false)
        return usage(*argv, "INFILE: file not found");

    fastgltf::Options options = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::GenerateMeshIndices | fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages;
    auto data = fastgltf::GltfDataBuffer::FromPath(infile);
    if (data.error() != fastgltf::Error::None)
        return usage(*argv, "INFILE: invalid glTF stream");
    auto asset = parser.loadGltf(data.get(), infile.parent_path(), options);
    if (asset.error() != fastgltf::Error::None)
        return usage(*argv, "INFILE: invalid glTF data");

    ImageGenerator makeimage;
    makeimage.set_uastc(enable_uastc);

    fbs::AssetsT out_assets;
    BufferTrain out_data;
    load_materials(out_assets, asset->materials, asset->textures);
    std::vector<std::vector<uint32_t>> mesh_groups = load_meshes(out_assets, out_data, asset.get(), max_uv_channels, max_color_channels, max_joint_channels);
    std::vector<size_t> skin_to_skeleton = load_skins(out_assets, out_data, asset.get());
    load_animations(out_assets, out_data, asset.get(), skin_to_skeleton);
    load_images(out_assets, out_data, asset.get(), makeimage, outparam);
    load_scene(out_assets, asset.get(), mesh_groups, skin_to_skeleton);
    load_names(out_assets, asset.get());

    flatbuffers::FlatBufferBuilder fbb;
    for (size_t i = 0; i < out_data.count(); i++)
        out_assets.buffers.push_back(out_data.size(i));
    fbb.Finish(fbs::Assets::Pack(fbb, &out_assets));

    std::ofstream ofs;
    fs::path ofs_path = outparam;
    ofs_path.replace_extension(".tgs");
    ofs.open(ofs_path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    ofs.close();

    out_data.dump(ofs_path);
    ofs.close();
    return 0;
}
