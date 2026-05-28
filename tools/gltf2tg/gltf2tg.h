#pragma once
#include <filesystem>
#include <span>
#include <volk.h>

class ImageGenerator {
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

    void generate(std::ostream& out, void* image_data, int width, int height, VkFormat format);

public:
    ImageGenerator();
    ~ImageGenerator();

    inline void set_uastc(bool enable_uastc) { m_enable_uastc = enable_uastc; }
    void generate(std::ostream& out, std::span<std::byte> image_data, std::string_view mimetype);
    void generate(std::ostream& out, const std::filesystem::path& in);
};
