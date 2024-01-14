#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <bitset>
#include <exception>
#include <string>
#include <vector>
#include <vk_mem_alloc.h>
#include "vkutil.h"

namespace twogame {
class Renderer;
}

namespace twogame::xml::assets {
struct Image;
struct Mesh;
struct Shader;
}

namespace twogame::asset {

enum class Type {
    Mesh,
    Image,
    Shader,
    MAX_VALUE
};

class AbstractAsset {
protected:
    const Renderer& m_renderer;

public:
    AbstractAsset(const Renderer* r)
        : m_renderer(*r)
    {
    }

    virtual Type type() const = 0;

    //! Issue commands in this method to prepare the asset for use. For example, place the asset in device-local memory here.
    virtual void prepare(VkCommandBuffer cmd) = 0;

    //! Clean up any resources that aren't needed once the asset is prepared.
    virtual void post_prepare() = 0;

    //! Is the asset prepared and ready for use?
    virtual bool prepared() const = 0;
};

class IOException final : public std::exception {
    std::string_view m_path;
    int m_errcode;

public:
    IOException(std::string_view path, int errcode)
        : m_path(path)
        , m_errcode(errcode)
    {
    }

    std::string_view path() const { return m_path; }
    int errcode() const { return m_errcode; }
};

class MalformedException final : public std::exception {
    std::string_view m_name;
    std::string m_what;

public:
    MalformedException(std::string_view name, const std::string& what)
        : m_name(name)
        , m_what(what)
    {
    }

    std::string_view name() const { return m_name; }
    std::string_view description() const { return m_what; }
};

class Image : public AbstractAsset {
private:
    VkBuffer m_storage;
    VkImage m_image;
    VkImageView m_view;
    VmaAllocation m_storage_mem, m_image_mem;
    uint32_t m_mip_levels, m_array_layers;
    std::vector<VkBufferImageCopy> m_copies;

public:
    Image(const xml::assets::Image&, const Renderer*);
    Image(const Image&) = delete;
    Image(Image&&) noexcept;
    virtual ~Image();

    const VkImage& image_handle() const { return m_image; }
    const VkImageView& image_view() const { return m_view; }

    virtual Type type() const { return Type::Image; }
    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;
};

class Mesh : public AbstractAsset {
public:
    struct VertexInputAttribute {
        uint32_t binding, offset;
        vk::VertexInput field;
        VkFormat format;
    };

private:
    VkBuffer m_buffer, m_staging;
    VmaAllocation m_buffer_mem, m_staging_mem;
    VkDeviceSize m_buffer_size;

    std::vector<VertexInputAttribute> m_attributes;
    std::vector<VkVertexInputBindingDescription> m_bindings;
    std::vector<VkDeviceSize> m_binding_offsets;
    VkPrimitiveTopology m_primitive_topology;
    VkDeviceSize m_index_offset;
    size_t m_index_count;
    VkIndexType m_index_type;
    uint64_t m_pipeline_parameter;

public:
    Mesh(const xml::assets::Mesh&, const Renderer*);
    Mesh(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept;
    virtual ~Mesh();

    inline const std::vector<VertexInputAttribute>& input_attributes() const { return m_attributes; }
    inline const std::vector<VkVertexInputBindingDescription>& input_bindings() const { return m_bindings; }
    inline VkPrimitiveTopology primitive_topology() const { return m_primitive_topology; }
    inline size_t index_count() const { return m_index_count; }
    inline uint64_t pipeline_parameter() const { return m_pipeline_parameter; }
    void bind_buffers(VkCommandBuffer cmd);

    virtual Type type() const { return Type::Mesh; }
    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;
};

class Shader : public AbstractAsset {
public:
    struct DescriptorSetSlot {
        uint32_t binding, offset, size, count;
        VkDescriptorType type;
        DescriptorSetSlot(uint32_t binding, VkDescriptorType type, uint32_t count, uint32_t offset, uint32_t size)
            : binding(binding)
            , offset(offset)
            , size(size)
            , count(count)
            , type(type)
        {
        }
    };

private:
    std::vector<VkPipelineShaderStageCreateInfo> m_stages;
    std::map<vk::VertexInput, int> m_inputs;
    std::map<std::string, DescriptorSetSlot> m_material_bindings;
    vk::DescriptorPool* m_descriptor_pool;
    VkPipelineLayout m_pipeline_layout;
    VkPipeline m_compute_pipeline;
    std::map<uint64_t, VkPipeline> m_graphics_pipelines;

public:
    Shader(const xml::assets::Shader&, const Renderer*);
    Shader(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    virtual ~Shader();

    virtual Type type() const { return Type::Shader; }
    virtual void prepare(VkCommandBuffer cmd) { }
    virtual void post_prepare() { }
    virtual bool prepared() const { return true; }

    const std::vector<VkPipelineShaderStageCreateInfo>& stages() const { return m_stages; }
    const std::map<std::string, DescriptorSetSlot>& material_bindings() const { return m_material_bindings; }
    vk::DescriptorPool* material_descriptor_pool() const { return m_descriptor_pool; }
    VkPipelineLayout pipeline_layout() const { return m_pipeline_layout; }
    VkPipeline compute_pipeline() const { return m_compute_pipeline; }
    VkPipeline graphics_pipeline(const asset::Mesh*);
};

}
