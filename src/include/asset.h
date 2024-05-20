#pragma once
#define VK_NO_PROTOTYPES
#include <array>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "render.h"
#include "xml/asset.h"

#ifdef TWOGAME_DEBUG_BUILD
#include <spdlog/fmt/fmt.h>
#endif

template <typename... Bases>
struct overload : Bases... {
    using is_transparent = void;
    using Bases::operator()...;
};

namespace twogame::asset {

template <typename T>
class lookup : public std::unordered_map<std::string, T, overload<std::hash<std::string>, std::hash<std::string_view>>, std::equal_to<>> {
};
template <>
class lookup<void> : public std::unordered_set<std::string, overload<std::hash<std::string>, std::hash<std::string_view>>, std::equal_to<>> {
};
class AssetManager;
class Mesh;
class Shader;

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
    std::string m_path;
    int m_errcode;

public:
    IOException(std::string_view path, int errcode)
        : m_path { path }
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
    template <typename... Ts>
    MalformedException(std::string_view name, std::string_view fmtstr, Ts&&... args)
        : m_name(name)
    {
#ifdef TWOGAME_DEBUG_BUILD
        m_what = fmt::vformat(fmtstr, fmt::make_format_args(args...));
#endif
    }

    std::string_view name() const { return m_name; }
    std::string_view description() const { return m_what; }
};

class Animation {
public:
    enum class ChannelTarget : uint8_t {
        Translation,
        Orientation,
        ShapeWeights,
    };
    enum class InterpolateMethod : uint8_t {
        Step,
        Linear,
        CubicSpline,
    };

private:
    struct Channel {
        float* m_data;
        uint32_t m_bone, m_width;
        ChannelTarget m_target;
    };
    std::vector<Channel> m_channels;
    std::unique_ptr<float[]> m_data;
    size_t m_keyframes;
    InterpolateMethod m_interp;

public:
    Animation(const xml::assets::Animation&, const AssetManager&);
    size_t channels() const { return m_channels.size(); }
    float duration() const { return m_data[m_keyframes - 1]; }
    bool is_shapekey() const;
    bool is_skeleton() const;

    class Iterator {
        const Animation* m_animation;
        std::vector<Channel>::const_iterator m_it;
        size_t m_index;
        float m_iv;

    public:
        Iterator(const Animation*, float t);
        Iterator& operator++();
        bool finished() const;

        inline ChannelTarget target() const { return m_it->m_target; }
        inline uint32_t bone() const { return m_it->m_bone; }

        void get(float*, size_t count) const;

        template <typename T>
        void get(T&) const;
    };

    Iterator interpolate(float t) const;
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
    Image(const xml::assets::Image&, const AssetManager&);
    Image(const Image&) = delete;
    Image(Image&&) noexcept;
    virtual ~Image();

    const VkImage& image_handle() const { return m_image; }
    const VkImageView& image_view() const { return m_view; }

    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;
};

class Material : public std::enable_shared_from_this<Material> {
private:
    struct Proto;
    struct MaterialImpl;

    const Renderer& m_renderer;
    std::shared_ptr<const Shader> m_shader;
    const bool m_mutable;

    std::shared_ptr<Proto> m_proto;
    std::unique_ptr<MaterialImpl[]> p_impl;

    Material(const Material&, std::true_type mut);
    void write_descriptor_sets() const;

public:
    Material(const xml::assets::Material&, const AssetManager&);
    Material(Material&&) noexcept;
    ~Material();

    std::shared_ptr<Material> get_mutable() const;
    const Shader* shader() const { return m_shader.get(); }
    const VkDescriptorSet& descriptor_set(int frame) const;
    VkPipeline pipeline(const Mesh*, size_t primitive_group) const;
};

class Mesh : public IPreparable {
public:
    struct PrimitiveGroup {
        std::vector<std::string_view> attribute_names;
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkDeviceSize> binding_offsets;
        size_t vertex_count, index_count;
        uint64_t pipeline_parameter;
    };

private:
    struct PrepareData;

    const Renderer& m_renderer;
    VkBuffer m_buffer;
    VkImage m_morph;
    VmaAllocation m_buffer_mem, m_morph_mem;
    VkImageView m_morph_position, m_morph_normal;

    size_t m_primitive_groups;
    std::unique_ptr<PrepareData> m_prepare_data;
    std::unique_ptr<PrimitiveGroup[]> m_primitives;
    VkIndexType m_index_buffer_width;
    VkPrimitiveTopology m_primitive_topology;

    lookup<void> m_attribute_names;
    std::vector<float> m_morph_weights;

public:
    Mesh(const xml::assets::Mesh&, const AssetManager&);
    Mesh(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept;
    virtual ~Mesh();

    inline VkPrimitiveTopology primitive_topology() const { return m_primitive_topology; }
    inline const PrimitiveGroup& primitive_group(size_t i) const { return m_primitives[i]; }
    inline const std::vector<float>& morph_weights() const { return m_morph_weights; }
    inline VkImageView morph_position() const { return m_morph_position; }
    inline VkImageView morph_normal() const { return m_morph_normal; }

    virtual void prepare(VkCommandBuffer cmd);
    virtual void post_prepare();
    virtual bool prepared() const;

    void draw(VkCommandBuffer cmd, uint64_t frame_number, const std::vector<std::shared_ptr<Material>>& materials) const;
};

class Skeleton {
public:
    struct BoneProto {
        uint32_t parent;
        vec3s translation;
        versors orientation;
    };

private:
    std::vector<mat4s> m_inverse_bind_matrices;
    std::vector<BoneProto> m_default_pose;

public:
    Skeleton(const xml::assets::Skeleton&, const AssetManager&);
    Skeleton(const Skeleton&) = delete;
    Skeleton(Skeleton&&) noexcept;
    ~Skeleton() { }

    size_t bones() const { return m_default_pose.size(); }
    const std::vector<BoneProto>& default_pose() const { return m_default_pose; }
    const std::vector<mat4s>& inverse_bind_matrices() const { return m_inverse_bind_matrices; }
};

class Shader {
public:
    struct MaterialBinding {
        uint32_t binding, offset, range;
    };
    struct DescriptorBinding {
        VkDescriptorType type;
        uint32_t offset, range;
    };

private:
    const Renderer& m_renderer;
    std::vector<VkPipelineShaderStageCreateInfo> m_stages;
    lookup<uint32_t> m_input_attributes;
    lookup<MaterialBinding> m_material_bindings;
    std::vector<DescriptorBinding> m_descriptor_bindings;
    size_t m_material_buffer_size;

    VkPipelineLayout m_pipeline_layout;
    VkPipeline m_compute_pipeline;
    std::unique_ptr<vk::DescriptorPool> m_descriptor_pool;
    mutable std::map<uint64_t, VkPipeline> m_graphics_pipelines;

public:
    Shader(const xml::assets::Shader&, const AssetManager&);
    Shader(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    virtual ~Shader();

    VkPipelineLayout pipeline_layout() const { return m_pipeline_layout; }
    VkPipeline compute_pipeline() const { return m_compute_pipeline; }
    const auto& input_attributes() const { return m_input_attributes; }

    vk::DescriptorPool* descriptor_pool() const { return m_descriptor_pool.get(); }
    const auto& descriptor_bindings() const { return m_descriptor_bindings; }
    size_t material_buffer_size() const { return m_material_buffer_size; }
    const auto& material_bindings() const { return m_material_bindings; }
    VkPipeline graphics_pipeline(const Mesh*, const Material*, size_t primitive_group) const;
};

class AssetManager {
private:
    const Renderer& m_renderer;
    std::deque<asset::IPreparable*> m_assets_preparing;
    lookup<std::shared_ptr<asset::Animation>> m_animations;
    lookup<std::shared_ptr<asset::Image>> m_images;
    lookup<std::shared_ptr<asset::Material>> m_materials;
    lookup<std::shared_ptr<asset::Mesh>> m_meshes;
    lookup<std::shared_ptr<asset::Shader>> m_shaders;
    lookup<std::shared_ptr<asset::Skeleton>> m_skeletons;

public:
    AssetManager(const Renderer*);
    bool import_assets(std::string_view path);
    size_t prepare(VkCommandBuffer cmd);
    void post_prepare();

    const Renderer& renderer() const { return m_renderer; }
    const auto& animations() const { return m_animations; }
    const auto& images() const { return m_images; }
    const auto& materials() const { return m_materials; }
    const auto& meshes() const { return m_meshes; }
    const auto& shaders() const { return m_shaders; }
    const auto& skeletons() const { return m_skeletons; }
};

}
