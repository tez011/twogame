#include "material.h"
#include "render/staging_buffer.h"
#include "scene.fbs.hpp"
#include "scene/scene.h"
#include "scene/scene_manifest.h"

namespace twogame::asset {

Material::Material(const SceneManifest& source, size_t source_index, size_t dst_index)
{
    const fbs::Material* info = source.manifest()->materials()->Get(source_index);
    memcpy(&m_entry.base_color_factor, info->base_color_factor()->v(), sizeof(vec4));
    memcpy(&m_entry.emissive_factor, info->emissive_factor()->v(), sizeof(vec3));
    m_entry.metallic_factor = info->metallic_factor();
    m_entry.roughness_factor = info->roughness_factor();
    m_entry.alpha_cutoff = info->alpha_cutoff();
    m_entry.base_color_texture = info->base_color_texture() ? info->base_color_texture()->image() : 0;
    m_entry.metallic_roughness_texture = info->metallic_roughness_texture() ? info->metallic_roughness_texture()->image() : 0;
    m_entry.normal_texture = info->normal_texture() ? info->normal_texture()->image() : 0;
    m_entry.occlusion_texture = info->occlusion_texture() ? info->occlusion_texture()->image() : 0;
    m_entry.base_color_uv = info->base_color_uv();
    m_entry.metallic_roughness_uv = info->metallic_roughness_uv();
    m_entry.normal_uv = info->normal_uv();
    m_entry.occlusion_uv = info->occlusion_uv();
    m_entry.emissive_uv = info->emissive_uv();

    m_prepared = std::make_shared<size_t>(dst_index);
}

Material::~Material()
{
}

void Material::advance_images(size_t image_count)
{
    m_entry.base_color_texture += image_count;
    m_entry.metallic_roughness_texture += image_count;
    m_entry.normal_texture += image_count;
    m_entry.occlusion_texture += image_count;
}

size_t Material::prepare(IScene* scene, StagingBuffer& commands)
{
    std::span<MaterialEntry> materials = scene->material_entries();
    size_t* dst_index = static_cast<size_t*>(std::get<std::shared_ptr<void>>(m_prepared).get());
    if (materials.empty()) {
        VkBufferCopy2 copy {};
        copy.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        copy.srcOffset = commands.tail_offset();
        copy.dstOffset = sizeof(MaterialEntry) * *dst_index;
        copy.size = sizeof(MaterialEntry);
        memcpy(commands.tail(), &m_entry, sizeof(MaterialEntry));
        commands.copy_buffer(scene->material_entries_buffer(), std::span(&copy, 1), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        return sizeof(MaterialEntry);
    } else {
        memcpy(materials.subspan(*dst_index).data(), &m_entry, sizeof(MaterialEntry));
        return 0;
    }
}

}
