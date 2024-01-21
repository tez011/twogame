#include "asset.h"
#include "render.h"
#include "twogame.h"
#include "xml.h"

namespace twogame::asset {

Material::Material(const xml::assets::Material& info, const AssetManager& assets)
{
    auto shader_it = assets.shaders().find(info.shader());
    m_shader = shader_it->second;
    m_shader->m_descriptor_pool->allocate(&m_descriptor_set);

    m_buffers.reserve(m_shader->m_buffer_pools.size());
    for (auto it = m_shader->m_buffer_pools.begin(); it != m_shader->m_buffer_pools.end(); ++it)
        m_buffers.emplace_back(it->first, it->second.allocate());

    for (auto it = info.props().begin(); it != info.props().end(); ++it) {
        auto mbi = m_shader->m_material_bindings.find(it->first);
        if (mbi == m_shader->m_material_bindings.end()) {
            spdlog::warn("material entry '{}' not present in shader '{}'", it->first, info.shader());
            continue;
        }

        auto& dsi = mbi->second;
        if (dsi.descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            std::vector<std::string_view> image_names;
            xml::priv::split(image_names, it->second, " ");
            if (image_names.size() != dsi.count) {
                spdlog::error("descriptor {} has count={} but {} were given", it->first, dsi.count, image_names.size());
                continue;
            }

            auto image_info = new VkDescriptorImageInfo[dsi.count];
            for (uint32_t i = 0; i < dsi.count; i++) {
                auto ii = assets.images().find(image_names[i]);
                image_info[i].sampler = m_shader->m_renderer.active_sampler();
                image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (ii == assets.images().end()) {
                    spdlog::error("unknown image {} referenced in material", image_names[i]);
                    image_info[i].imageView = VK_NULL_HANDLE;
                } else {
                    image_info[i].imageView = static_cast<asset::Image*>(ii->second.get())->image_view();
                }
            }

            VkWriteDescriptorSet& w = m_descriptor_writes.emplace_back();
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m_descriptor_set;
            w.dstBinding = dsi.binding;
            w.dstArrayElement = 0;
            w.descriptorCount = dsi.count;
            w.descriptorType = dsi.descriptor_type;
            w.pImageInfo = image_info;
        } else if (dsi.descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (dsi.count != 1) {
                spdlog::error("descriptor {} has count={} but uniform/storage arrays are not supported in materials", it->first, dsi.count);
                continue;
            }

            auto& buffer_pool = m_shader->m_buffer_pools.at(dsi.binding);
            auto buffer_info = new VkDescriptorBufferInfo;
            size_t bin = buffer_pool.allocate();
            buffer_pool.buffer_handle(bin, *buffer_info);
            void* bl = buffer_pool.buffer_memory(bin, dsi.offset);
            if (dsi.field_type.parse(it->second, bl) == false) {
                spdlog::error("could not parse '{}' as field {}", it->second, it->first);
                delete buffer_info;
                continue;
            }

            VkWriteDescriptorSet& w = m_descriptor_writes.emplace_back();
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m_descriptor_set;
            w.dstBinding = dsi.binding;
            w.dstArrayElement = 0;
            w.descriptorCount = 1;
            w.descriptorType = dsi.descriptor_type;
            w.pBufferInfo = buffer_info;
        } else {
            spdlog::critical("unknown descriptor type {}; please file a bug", static_cast<size_t>(dsi.descriptor_type));
            std::terminate();
        }
    }
    vkUpdateDescriptorSets(m_shader->m_renderer.device(), m_descriptor_writes.size(), m_descriptor_writes.data(), 0, nullptr);
}

Material::Material(const Material& other)
    : m_shader(other.m_shader)
    , m_descriptor_writes(other.m_descriptor_writes)
{
    m_shader->m_descriptor_pool->allocate(&m_descriptor_set);

    m_buffers.reserve(m_shader->m_buffer_pools.size());
    for (auto it = m_shader->m_buffer_pools.begin(); it != m_shader->m_buffer_pools.end(); ++it)
        m_buffers.emplace_back(it->first, it->second.allocate());

    std::vector<VkCopyDescriptorSet> copies(m_descriptor_writes.size());
    for (size_t i = 0; i < copies.size(); i++) {
        copies[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
        copies[i].pNext = nullptr;
        copies[i].srcSet = other.m_descriptor_set;
        copies[i].srcBinding = other.m_descriptor_writes[i].dstBinding;
        copies[i].srcArrayElement = 0;
        copies[i].dstSet = m_descriptor_set;
        copies[i].dstBinding = other.m_descriptor_writes[i].dstBinding;
        copies[i].dstArrayElement = 0;
        copies[i].descriptorCount = other.m_descriptor_writes[i].descriptorCount;
    }
    vkUpdateDescriptorSets(m_shader->m_renderer.device(), 0, nullptr, copies.size(), copies.data());

    for (auto it = m_descriptor_writes.begin(); it != m_descriptor_writes.end(); ++it) {
        if (it->pBufferInfo != nullptr) {
            auto new_buffer_info = new VkDescriptorBufferInfo;
            size_t bin = m_shader->m_buffer_pools.at(it->dstBinding).allocate();
            m_shader->m_buffer_pools.at(it->dstBinding).buffer_handle(bin, *new_buffer_info);
            it->pBufferInfo = new_buffer_info;
        }
        if (it->pImageInfo != nullptr) {
            auto new_image_info = new VkDescriptorImageInfo[it->descriptorCount];
            memcpy(new_image_info, it->pImageInfo, it->descriptorCount * sizeof(VkDescriptorImageInfo));
            it->pImageInfo = new_image_info;
        }
    }
}

Material::Material(Material&& other) noexcept
    : m_shader(std::move(other.m_shader))
    , m_buffers(std::move(other.m_buffers))
    , m_descriptor_writes(std::move(other.m_descriptor_writes))
    , m_descriptor_set(other.m_descriptor_set)
{
    other.m_descriptor_set = VK_NULL_HANDLE;
}

Material::~Material()
{
    for (VkWriteDescriptorSet& w : m_descriptor_writes) {
        if (w.pImageInfo != nullptr)
            delete[] w.pImageInfo;
        if (w.pBufferInfo != nullptr)
            delete w.pBufferInfo;
    }

    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it)
        m_shader->m_buffer_pools.at(it->first).free(it->second);
    m_shader->m_descriptor_pool->free(&m_descriptor_set);
}

}
