#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <cglm/struct.h>
#include <fastgltf/tools.hpp>
#include <volk.h>

class ImageGenerator {
public:
    class SerializedImage {
        friend class ImageGenerator;

        std::unique_ptr<const std::byte, void (*)(const void*)> m_data;
        size_t m_size;

        SerializedImage(const void* data, size_t size);

    public:
        SerializedImage()
            : m_data(nullptr, [](const void*) {})
            , m_size(0)
        {
        }
        SerializedImage(SerializedImage&& other)
            : m_data(std::move(other.m_data))
            , m_size(other.m_size)
        {
        }
        ~SerializedImage() { }
        std::span<const std::byte> as_bytes() const
        {
            return std::span(m_data.get(), m_size);
        }
    };

private:
    static PFN_vkDestroyDebugUtilsMessengerEXT s_vkDestroyDebugUtilsMessenger;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_hwd = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_command_pool;
    VkQueue m_queue;
    VkShaderModule m_shader;
    VkDescriptorSetLayout m_descriptor_layout;
    VkPipelineLayout m_pipeline_layout;
    VkPipeline m_pipeline;

    bool m_enable_uastc = false;
    void* m_debugger = nullptr;

    void create_instance();
    void create_debug_messenger();
    void pick_physical_device();
    void create_logical_device();
    void create_pipeline();
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags flags);

    SerializedImage generate(void* image_data, int width, int height, VkFormat format);

public:
    ImageGenerator();
    ~ImageGenerator();

    inline void set_uastc(bool enable_uastc) { m_enable_uastc = enable_uastc; }
    SerializedImage generate(std::span<const std::byte> image_data);
    SerializedImage generate(const std::filesystem::path& in);
};
