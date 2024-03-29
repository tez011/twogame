#pragma once
#include <filesystem>
#include <volk.h>

class ImageGenerator {
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

    static PFN_vkDestroyDebugUtilsMessengerEXT s_vkDestroyDebugUtilsMessenger;
    void create_instance();
    void create_debug_messenger();
    void pick_physical_device();
    void create_logical_device();
    void create_pipeline();
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags);

    void generate(const std::filesystem::path& out, void* raw_image_data, int w, int h, VkFormat input_format);

public:
    ImageGenerator();
    ~ImageGenerator();

    void set_enable_uastc(bool enable) { m_enable_uastc = enable;}
    void generate(const std::filesystem::path& out, const unsigned char* image_data, size_t image_len, std::string_view mimetype);
    void generate(const std::filesystem::path& out, const std::filesystem::path& in);
};

