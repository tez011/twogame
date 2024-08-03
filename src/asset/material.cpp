#include <list>
#include <set>
#include "asset.h"
#include "fast_float/fast_float.h"
#include "render.h"
#include "xml/asset.h"

namespace twogame::asset {
static_assert(sizeof(float) == 4);
static const char* BINDING_SEPARATORS = " \n\t\v\f\r";

template <typename T>
static bool parse_binding_field(std::string_view text, T* dst)
{
    return fast_float::from_chars(text.data(), text.data() + text.size(), *dst).ec == std::errc();
}

static bool parse_binding_field(std::string_view text, std::string_view type, char* dst, size_t index)
{
    if (type == "int")
        return parse_binding_field(text, reinterpret_cast<uint32_t*>(dst) + index);
    else if (type == "float")
        return parse_binding_field(text, reinterpret_cast<float*>(dst) + index);
    else
        return false;
}

struct Material::MaterialImpl {
    VkBuffer buffer;
    VmaAllocation allocation;
    VkDescriptorSet descriptor;
};
struct Material::Proto {
    std::vector<bool> binding_contents;
    std::map<uint32_t, std::shared_ptr<const Image>> images;

    Proto(size_t binding_count)
        : binding_contents(binding_count)
    {
    }
};

Material::Material(const xml::assets::Material& info, const AssetManager& manager)
    : m_renderer(manager.renderer())
    , m_mutable(false)
{
    auto shader_it = manager.shaders().find(info.shader());
    if (shader_it == manager.shaders().end())
        throw MalformedException(info.name(), "no such shader '{}'", info.shader());

    m_shader = shader_it->second;
    m_proto = std::make_shared<Proto>(m_shader->descriptor_bindings().size());
    p_impl = std::make_unique<MaterialImpl[]>(1);

    std::unique_ptr<char[]> local_buffer = std::make_unique<char[]>(m_shader->material_buffer_size());
    for (const auto& p : info.props()) {
        auto binding_it = m_shader->material_bindings().find(p.name());
        if (binding_it == m_shader->material_bindings().end())
            continue;

        const Shader::MaterialBinding& binding = binding_it->second;
        m_proto->binding_contents[binding.binding] = true;
        if (p.type() == "image-sampler") {
            auto ii = manager.images().find(p.value());
            if (ii == manager.images().end())
                throw MalformedException(info.name(), "no such image '{}'", p.value());

            m_proto->images[binding.binding] = ii->second;
        } else {
            std::string_view::size_type start = 0, end;
            size_t tokens = 0, max_tokens = binding.range / 4, buffer_offset = m_shader->descriptor_bindings().at(binding.binding).offset + binding.offset;
            while ((end = p.value().find_first_of(BINDING_SEPARATORS), start) != std::string_view::npos && tokens < max_tokens) {
                if (!parse_binding_field(p.value().substr(start, end - start), p.type(), local_buffer.get() + buffer_offset, tokens++))
                    throw MalformedException(info.name(), "failed to parse material binding {}", p.name());
                start = p.value().find_first_not_of(BINDING_SEPARATORS, end + 1);
            }
            if (start != std::string_view::npos && tokens < max_tokens) {
                if (!parse_binding_field(p.value().substr(start), p.type(), local_buffer.get() + buffer_offset, tokens++))
                    throw MalformedException(info.name(), "failed to parse material binding {}", p.name());
            }
        }
    }

    if (m_shader->material_buffer_size()) {
        void* live_buffer;
        VkBufferCreateInfo buffer_ci {};
        buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_ci.size = m_shader->material_buffer_size();
        buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo alloc_ci {};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &p_impl[0].buffer, &p_impl[0].allocation, nullptr));
        VK_CHECK(vmaMapMemory(m_renderer.allocator(), p_impl[0].allocation, &live_buffer));
        memcpy(live_buffer, local_buffer.get(), m_shader->material_buffer_size());
        vmaUnmapMemory(m_renderer.allocator(), p_impl[0].allocation);
    } else {
        p_impl[0].buffer = VK_NULL_HANDLE;
    }

    m_shader->descriptor_pool()->allocate(&p_impl[0].descriptor);
    write_descriptor_sets();
}

Material::Material(const Material& other, std::true_type mut)
    : m_renderer(other.m_renderer)
    , m_shader(other.m_shader)
    , m_mutable(mut)
    , m_proto(other.m_proto)
{
    void* source_buffer;
    p_impl = std::make_unique<MaterialImpl[]>(2);

    if (m_shader->material_buffer_size()) {
        void* live_buffer;
        VkBufferCreateInfo buffer_ci {};
        buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_ci.size = m_shader->material_buffer_size();
        buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo alloc_ci {};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &p_impl[0].buffer, &p_impl[0].allocation, nullptr));
        VK_CHECK(vmaMapMemory(m_renderer.allocator(), other.p_impl[0].allocation, &source_buffer));
        VK_CHECK(vmaMapMemory(m_renderer.allocator(), p_impl[0].allocation, &live_buffer));
        memcpy(live_buffer, source_buffer, m_shader->material_buffer_size());
        vmaUnmapMemory(m_renderer.allocator(), p_impl[0].allocation);
        if (other.m_mutable) {
            VK_CHECK(vmaMapMemory(m_renderer.allocator(), other.p_impl[1].allocation, &source_buffer));
            vmaUnmapMemory(m_renderer.allocator(), other.p_impl[0].allocation);
        }

        VK_CHECK(vmaCreateBuffer(m_renderer.allocator(), &buffer_ci, &alloc_ci, &p_impl[1].buffer, &p_impl[1].allocation, nullptr));
        VK_CHECK(vmaMapMemory(m_renderer.allocator(), p_impl[1].allocation, &live_buffer));
        memcpy(live_buffer, source_buffer, m_shader->material_buffer_size());
        vmaUnmapMemory(m_renderer.allocator(), p_impl[1].allocation);
        vmaUnmapMemory(m_renderer.allocator(), other.p_impl[other.m_mutable ? 1 : 0].allocation);
    } else {
        p_impl[0].buffer = p_impl[1].buffer = VK_NULL_HANDLE;
    }

    m_shader->descriptor_pool()->allocate(&p_impl[0].descriptor);
    m_shader->descriptor_pool()->allocate(&p_impl[1].descriptor);
    write_descriptor_sets();
}

Material::Material(Material&& other) noexcept
    : m_renderer(other.m_renderer)
    , m_shader(std::move(other.m_shader))
    , m_mutable(other.m_mutable)
    , m_proto(std::move(other.m_proto))
    , p_impl(std::move(other.p_impl))
{
}

Material::~Material()
{
    if (m_mutable) {
        m_shader->descriptor_pool()->free(&p_impl[1].descriptor);
        vmaDestroyBuffer(m_renderer.allocator(), p_impl[1].buffer, p_impl[1].allocation);
    }
    m_shader->descriptor_pool()->free(&p_impl[0].descriptor);
    vmaDestroyBuffer(m_renderer.allocator(), p_impl[0].buffer, p_impl[0].allocation);
}

void Material::write_descriptor_sets() const
{
    std::vector<VkWriteDescriptorSet> writes;
    std::list<VkDescriptorBufferInfo> buffers;
    std::list<VkDescriptorImageInfo> images;
    writes.reserve(m_proto->binding_contents.size());

    for (size_t i = 0; i < m_proto->binding_contents.size(); i++) {
        if (!m_proto->binding_contents[i])
            continue;

        VkWriteDescriptorSet& w = writes.emplace_back();
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p_impl[0].descriptor;
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = m_shader->descriptor_bindings().at(i).type;
        switch (w.descriptorType) {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            VkDescriptorImageInfo& iinfo = images.emplace_back();
            iinfo.sampler = m_renderer.active_sampler();
            iinfo.imageView = m_proto->images[i]->image_view();
            iinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w.pImageInfo = &iinfo;
        } break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            VkDescriptorBufferInfo& binfo = buffers.emplace_back();
            binfo.buffer = p_impl[0].buffer;
            binfo.offset = m_shader->descriptor_bindings().at(i).offset;
            binfo.range = p_impl[0].buffer ? m_shader->descriptor_bindings().at(i).range : VK_WHOLE_SIZE;
            w.pBufferInfo = &binfo;
        } break;
        default:
            assert(false);
        }
    }
    vkUpdateDescriptorSets(m_renderer.device(), writes.size(), writes.data(), 0, nullptr);

    if (m_mutable) {
        for (auto it = writes.begin(); it != writes.end(); ++it)
            it->dstSet = p_impl[1].descriptor;
        for (auto it = buffers.begin(); it != buffers.end(); ++it)
            it->buffer = p_impl[1].buffer;
        vkUpdateDescriptorSets(m_renderer.device(), writes.size(), writes.data(), 0, nullptr);
    }
}

std::shared_ptr<Material> Material::get_mutable() const
{
    Material child(*this, std::true_type());
    return child.shared_from_this();
}

const VkDescriptorSet& Material::descriptor_set(int frame) const
{
    if (m_mutable)
        return p_impl[frame % 2].descriptor;
    else
        return p_impl[0].descriptor;
}

VkPipeline Material::pipeline(const Mesh* mesh, size_t primitive_group) const
{
    return m_shader->graphics_pipeline(mesh, this, primitive_group);
}

}
