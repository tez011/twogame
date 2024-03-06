#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <bitset>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cglm/struct.h>
#include <vk_mem_alloc.h>
#include "vkutil.h"

template <typename... Bases>
struct overload : Bases... {
    using is_transparent = void;
    using Bases::operator()...;
};

struct SpvReflectTypeDescription;

namespace twogame {
class Renderer;
}

namespace twogame::xml::assets {
struct Animation;
struct Image;
struct Material;
struct Mesh;
struct Shader;
}

namespace twogame::asset {

enum class Type {
    Material,
    Mesh,
    Image,
    Shader,
    MAX_VALUE
};
template <typename T>
class lookup : public std::unordered_map<std::string, T, overload<std::hash<std::string>, std::hash<std::string_view>>, std::equal_to<>> { };
class AssetManager;
class Animation;

class IPreparable {
public:
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

class Image : public IPreparable {
private:
    const Renderer& m_renderer;
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

    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;
};

class Mesh : public IPreparable {
public:
    struct VertexInputAttribute {
        uint32_t binding, offset;
        vk::VertexInput field;
        VkFormat format;
    };
    struct DefaultJoint {
        vec3s translation;
        versors orientation;
        uint32_t parent;
    };

private:
    const Renderer& m_renderer;
    VkBuffer m_buffer, m_staging;
    VmaAllocation m_buffer_mem, m_staging_mem;
    VkDeviceSize m_buffer_size;

    VkImage m_displacement_image;
    VkImageView m_displacement_position, m_displacement_normal;
    VmaAllocation m_displacement_mem;
    VkBufferImageCopy m_displacement_prep;
    std::vector<float> m_displacement_initial_weights;

    std::vector<mat4s> m_inverse_bind_matrices;
    std::vector<DefaultJoint> m_default_pose;

    std::vector<VertexInputAttribute> m_attributes;
    std::vector<VkVertexInputBindingDescription> m_bindings;
    std::vector<VkDeviceSize> m_binding_offsets;
    VkPrimitiveTopology m_primitive_topology;
    size_t m_index_count;
    VkIndexType m_index_type;
    uint64_t m_pipeline_parameter;

    lookup<std::shared_ptr<Animation>> m_animations;

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

    inline VkImageView position_displacement() const { return m_displacement_position; }
    inline VkImageView normal_displacement() const { return m_displacement_normal; }
    inline const std::vector<float>& displacement_initial_weights() const { return m_displacement_initial_weights; }
    inline const std::vector<mat4s>& inverse_bind_matrices() const { return m_inverse_bind_matrices; }
    inline const std::vector<DefaultJoint>& default_pose() const { return m_default_pose; }
    inline const auto& animations() const { return m_animations; }

    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;
};

class Shader {
    friend class Material;

public:
    class FieldType {
    private:
        union {
            struct {
                unsigned dim : 4; // scalar, vec{2,3,4}, mat{2,3,4}{2,3,4}
                unsigned st : 4; // void, bool, int32, int64, uint32, uint64, float32, float64
            } f;
            uint32_t rep;
        };

    public:
        FieldType()
            : rep(0)
        {
        }
        FieldType(SpvReflectTypeDescription*);
        bool parse(const std::string_view& text, void* out);
    };
    struct DescriptorSetSlot {
        uint32_t binding, offset, count;
        FieldType field_type;
        VkDescriptorType descriptor_type;
        DescriptorSetSlot(uint32_t binding, VkDescriptorType descriptor_type, FieldType field_type, uint32_t count, uint32_t offset)
            : binding(binding)
            , offset(offset)
            , count(count)
            , field_type(field_type)
            , descriptor_type(descriptor_type)
        {
        }
    };

private:
    const Renderer& m_renderer;
    std::vector<VkPipelineShaderStageCreateInfo> m_stages;
    std::map<vk::VertexInput, int> m_inputs;
    std::map<std::string, DescriptorSetSlot> m_material_bindings;
    vk::DescriptorPool* m_descriptor_pool;
    VkPipelineLayout m_pipeline_layout;
    VkPipeline m_compute_pipeline;
    std::map<uint32_t, vk::BufferPool> m_buffer_pools;
    mutable std::map<uint64_t, VkPipeline> m_graphics_pipelines;

public:
    Shader(const xml::assets::Shader&, const Renderer*);
    Shader(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    virtual ~Shader();

    VkPipelineLayout pipeline_layout() const { return m_pipeline_layout; }
    VkPipeline compute_pipeline() const { return m_compute_pipeline; }
    VkPipeline graphics_pipeline(const asset::Mesh*) const;
};

class Material {
private:
    std::shared_ptr<asset::Shader> m_shader;
    std::vector<std::pair<uint32_t, size_t>> m_buffers;
    std::vector<VkWriteDescriptorSet> m_descriptor_writes;
    VkDescriptorSet m_descriptor_set;

public:
    Material(const xml::assets::Material&, const AssetManager&);
    Material(const Material&);
    Material(Material&&) noexcept;
    ~Material();

    const asset::Shader* shader() const { return m_shader.get(); }
    VkDescriptorSet descriptor() const { return m_descriptor_set; }
};

class Animation {
public:
    enum class ChannelTarget : uint16_t {
        Translation,
        Orientation,
        DisplaceWeights,
        MAX_VALUE,
    };

private:
    struct Channel {
        std::vector<float> m_data;
        uint32_t m_bone;
        ChannelTarget m_target;
        bool m_step_interpolate;
    };
    std::vector<float> m_inputs;
    std::vector<Channel> m_channels;

public:
    Animation(const xml::assets::Animation&, Mesh* mesh);
    float duration() const { return m_inputs[m_inputs.size() - 1]; }

    class Iterator {
        friend class Animation;
        const Animation* m_animation;
        std::vector<Channel>::const_iterator m_it;
        size_t m_index;
        float m_iv;

        Iterator(const Animation*, float t);

    public:
        Iterator& operator++();

        inline ChannelTarget target() const { return m_it->m_target; }
        inline uint32_t bone() const { return m_it->m_bone; }
        void get(size_t count, float* out) const;
    };

    Iterator interpolate(float t) const;
    bool finished(const Iterator&) const;
};

class AssetManager {
private:
    std::deque<asset::IPreparable*> m_assets_preparing;
    lookup<std::shared_ptr<asset::Image>> m_images;
    lookup<std::shared_ptr<asset::Material>> m_materials;
    lookup<std::shared_ptr<asset::Mesh>> m_meshes;
    lookup<std::shared_ptr<asset::Shader>> m_shaders;
    lookup<std::shared_ptr<asset::Animation>> m_animations;

public:
    bool import_assets(std::string_view path, const Renderer*);
    size_t prepare(VkCommandBuffer cmd);
    void post_prepare();

    const auto& images() const { return m_images; }
    const auto& materials() const { return m_materials; }
    const auto& meshes() const { return m_meshes; }
    const auto& shaders() const { return m_shaders; }
    const auto& animations() const { return m_animations; }
};

}
