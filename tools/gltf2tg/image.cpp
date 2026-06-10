#define STBI_NO_BMP
#define STBI_NO_PSD
#define STB_IMAGE_IMPLEMENTATION
#define VK_ENABLE_BETA_EXTENSIONS
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <ktx.h>
#include "embedded_shaders.h"
#include "gltf2tg.h"
#include "stb_image.h"
#ifdef LINK_RENDERDOC
#include <dlfcn.h>
#include "renderdoc_app.h"
#endif

#ifdef DEBUG_BUILD
#include <vulkan/vk_enum_string_helper.h>
#define VK_DEMAND(X) VK_DEMAND_4P(X, __FILE__, __LINE__)
#define VK_DEMAND_4P(X, F, L)                                                                                      \
    do {                                                                                                           \
        VkResult __r;                                                                                              \
        if ((__r = (X)) != VK_SUCCESS) {                                                                           \
            std::cerr << "assert at " << F << ':' << L << ": " << #X << ": " << string_VkResult(__r) << std::endl; \
            std::abort();                                                                                          \
        }                                                                                                          \
    } while (0)
#else
#define VK_DEMAND(X)             \
    do {                         \
        if ((X) != VK_SUCCESS) { \
            std::abort();        \
        }                        \
    } while (0)
#endif
#ifdef DEBUG_BUILD
constexpr static bool ENABLE_VALIDATION_LAYERS = true;
constexpr static const char* INSTANCE_LAYERS[] = { "VK_LAYER_KHRONOS_validation" };
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 1;
#else
constexpr static bool ENABLE_VALIDATION_LAYERS = false;
constexpr static const char** INSTANCE_LAYERS = nullptr;
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 0;
#endif

PFN_vkDestroyDebugUtilsMessengerEXT ImageGenerator::s_vkDestroyDebugUtilsMessenger = nullptr;
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* cb_data, void* user_data)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        std::cerr << "[E] " << cb_data->pMessage << std::endl;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[W] " << cb_data->pMessage << std::endl;
#ifdef DEBUG_BUILD
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT && ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0))
        std::cerr << "[I] " << cb_data->pMessage << std::endl;
    else
        std::cerr << "[D] " << cb_data->pMessage << std::endl;

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        std::raise(SIGABRT);
#endif

    return VK_FALSE;
}

ImageGenerator::SerializedImage::SerializedImage(const void* data, size_t size)
    : m_data(reinterpret_cast<const std::byte*>(data), [](const void* p) { std::free(const_cast<void*>(p)); })
    , m_size(size)
{
}

ImageGenerator::ImageGenerator()
{
    create_instance();
    create_debug_messenger();
    pick_physical_device();
    create_logical_device();
    create_pipeline();

#ifdef LINK_RENDERDOC
    if (void* mod = dlopen("/usr/lib/librenderdoc.so", RTLD_NOW)) {
        pRENDERDOC_GetAPI s = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
        int ret = s(eRENDERDOC_API_Version_1_1_2, &m_debugger);
        assert(ret == 1);
    } else {
        fprintf(stderr, "dlopen: %s\n", dlerror());
    }
#else
    m_debugger = nullptr;
#endif
}

ImageGenerator::~ImageGenerator()
{
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_descriptor_layout, nullptr);
    vkDestroyShaderModule(m_device, m_shader, nullptr);
    vkDestroyCommandPool(m_device, m_command_pool, nullptr);
    vkDestroyDevice(m_device, nullptr);
    if (m_debug_messenger)
        s_vkDestroyDebugUtilsMessenger(m_instance, m_debug_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

void ImageGenerator::create_instance()
{
    unsigned int n;
    std::vector<const char*> instance_extensions;
    if (ENABLE_VALIDATION_LAYERS)
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags instance_create_flags = 0;
    std::vector<VkExtensionProperties> available_instance_extensions;
    VK_DEMAND(vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr));
    available_instance_extensions.resize(n);
    VK_DEMAND(vkEnumerateInstanceExtensionProperties(nullptr, &n, available_instance_extensions.data()));
    for (auto& ext : available_instance_extensions) {
        if (strcmp(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, ext.extensionName) == 0) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }

    VkApplicationInfo appinfo {};
    appinfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appinfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createinfo.flags = instance_create_flags;
    createinfo.pApplicationInfo = &appinfo;
    createinfo.enabledLayerCount = INSTANCE_LAYERS_COUNT;
    createinfo.ppEnabledLayerNames = INSTANCE_LAYERS;
    createinfo.enabledExtensionCount = instance_extensions.size();
    createinfo.ppEnabledExtensionNames = instance_extensions.data();
    VK_DEMAND(vkCreateInstance(&createinfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);
}

void ImageGenerator::create_debug_messenger()
{
    if (ENABLE_VALIDATION_LAYERS == false)
        return;

    auto vkCreateDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (s_vkDestroyDebugUtilsMessenger == nullptr)
        s_vkDestroyDebugUtilsMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkCreateDebugUtilsMessenger == nullptr || s_vkDestroyDebugUtilsMessenger == nullptr) {
        std::cerr << "[F] Requested extension " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " not present" << std::endl;
        std::abort();
    }

    VkDebugUtilsMessengerCreateInfoEXT createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createinfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createinfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createinfo.pfnUserCallback = vk_debug_callback;

    VK_DEMAND(vkCreateDebugUtilsMessenger(m_instance, &createinfo, nullptr, &m_debug_messenger));
}

void ImageGenerator::pick_physical_device()
{
    uint32_t device_count = 0;
    std::vector<VkPhysicalDevice> devices;
    vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());

    VkPhysicalDevice dGPU = VK_NULL_HANDLE, iGPU = VK_NULL_HANDLE;
    for (auto& device : devices) {
        VkPhysicalDeviceProperties device_props;
        vkGetPhysicalDeviceProperties(device, &device_props);
        m_hwd = device;
        if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            dGPU = device;
        } else if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            iGPU = device;
        }
    }

    // m_hwd is the last device that is barely usable; dGPU is the first discrete GPU; iGPU is the first integrated GPU.
    if (dGPU != VK_NULL_HANDLE)
        m_hwd = dGPU;
    else if (iGPU != VK_NULL_HANDLE)
        m_hwd = iGPU;
}

void ImageGenerator::create_logical_device()
{
    uint32_t count = 0;
    std::vector<VkExtensionProperties> available_exts;
    std::vector<const char*> extensions;
    if (m_hwd == VK_NULL_HANDLE) {
        std::cerr << "[F] no Vulkan devices were found on this machine" << std::endl;
        std::abort();
    }

    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, nullptr);
    available_exts.resize(count);
    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, available_exts.data());
    for (auto& ext : available_exts) {
        if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(m_hwd, &properties);
    std::cerr << "[I] selecting device " << properties.deviceName << std::endl;

    std::vector<VkQueueFamilyProperties> qf_properties;
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, nullptr);
    qf_properties.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, qf_properties.data());

    uint32_t qf_primary = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if ((qf_properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            qf_primary = i;
            break;
        }
    }

    VkDeviceQueueCreateInfo qci {};
    float one = 1;
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf_primary;
    qci.queueCount = 1;
    qci.pQueuePriorities = &one;

    VkDeviceCreateInfo createinfo {};
    VkPhysicalDeviceFeatures features {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createinfo.pNext = nullptr;
    createinfo.queueCreateInfoCount = 1;
    createinfo.pQueueCreateInfos = &qci;
    createinfo.enabledExtensionCount = extensions.size();
    createinfo.ppEnabledExtensionNames = extensions.data();
    createinfo.pEnabledFeatures = &features;
    VK_DEMAND(vkCreateDevice(m_hwd, &createinfo, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, qf_primary, 0, &m_queue);

    VkCommandPoolCreateInfo cmdpool_ci = {};
    cmdpool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdpool_ci.queueFamilyIndex = qf_primary;
    cmdpool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_DEMAND(vkCreateCommandPool(m_device, &cmdpool_ci, nullptr, &m_command_pool));
}

void ImageGenerator::create_pipeline()
{
    VkShaderModuleCreateInfo shader_ci {};
    shader_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_ci.codeSize = twogame::shaders::gltf2tg_resample_comp_size;
    shader_ci.pCode = twogame::shaders::gltf2tg_resample_comp_spv;
    VK_DEMAND(vkCreateShaderModule(m_device, &shader_ci, nullptr, &m_shader));

    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
    };
    VkDescriptorSetLayoutCreateInfo dlci {};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;
    VK_DEMAND(vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_descriptor_layout));

    VkPipelineLayoutCreateInfo layout_ci {};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &m_descriptor_layout;
    VK_DEMAND(vkCreatePipelineLayout(m_device, &layout_ci, nullptr, &m_pipeline_layout));

    VkComputePipelineCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    createinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    createinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    createinfo.stage.module = m_shader;
    createinfo.stage.pName = "main";
    createinfo.layout = m_pipeline_layout;
    VK_DEMAND(vkCreateComputePipelines(m_device, nullptr, 1, &createinfo, nullptr, &m_pipeline));
}

uint32_t ImageGenerator::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_hwd, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

static int stb_io_read(void* user, char* data, int size)
{
    std::ifstream* self = reinterpret_cast<std::ifstream*>(user);
    self->read(data, size);
    return self->gcount();
}

static void stb_io_skip(void* user, int n)
{
    std::ifstream* self = reinterpret_cast<std::ifstream*>(user);
    self->seekg(n, std::ios_base::cur);
}

static int stb_io_eof(void* user)
{
    std::ifstream* self = reinterpret_cast<std::ifstream*>(user);
    return self->eof();
}

ImageGenerator::SerializedImage ImageGenerator::generate(std::span<const std::byte> image_data)
{
    int width, height, n;
    const stbi_uc* image_ptr = reinterpret_cast<const stbi_uc*>(image_data.data());
    if (stbi_is_hdr_from_memory(image_ptr, image_data.size())) {
        float* rd = stbi_loadf_from_memory(image_ptr, image_data.size(), &width, &height, &n, 4);
        if (rd)
            return generate(rd, width, height, VK_FORMAT_R32G32B32A32_SFLOAT);
        else
            std::cerr << "[E] failed to decode image: " << stbi_failure_reason() << std::endl;
    } else if (stbi_is_16_bit_from_memory(image_ptr, image_data.size())) {
        stbi_us* rd = stbi_load_16_from_memory(image_ptr, image_data.size(), &width, &height, &n, 4);
        if (rd)
            return generate(rd, width, height, VK_FORMAT_R16G16B16A16_UINT);
        else
            std::cerr << "[E] failed to decode image: " << stbi_failure_reason() << std::endl;
    } else {
        stbi_uc* rd = stbi_load_from_memory(image_ptr, image_data.size(), &width, &height, &n, 4);
        if (rd)
            return generate(rd, width, height, VK_FORMAT_R8G8B8A8_SRGB);
        else
            std::cerr << "[E] failed to decode image: " << stbi_failure_reason() << std::endl;
    }
    return SerializedImage();
}

ImageGenerator::SerializedImage ImageGenerator::generate(const std::filesystem::path& in)
{
    int x, y, n;
    if (stbi_is_hdr(in.c_str())) {
        float* rd = stbi_loadf(in.c_str(), &x, &y, &n, 4);
        if (rd)
            return generate(rd, x, y, VK_FORMAT_R32G32B32A32_SFLOAT);
        else
            std::cerr << "[E] failed to decode image " << in << ": " << stbi_failure_reason() << std::endl;
    } else if (stbi_is_16_bit(in.c_str())) {
        stbi_us* rd = stbi_load_16(in.c_str(), &x, &y, &n, 4);
        if (rd)
            return generate(rd, x, y, VK_FORMAT_R16G16B16A16_UINT);
        else
            std::cerr << "[E] failed to decode image " << in << ": " << stbi_failure_reason() << std::endl;
    } else {
        stbi_uc* rd = stbi_load(in.c_str(), &x, &y, &n, 4);
        if (rd)
            return generate(rd, x, y, VK_FORMAT_R8G8B8A8_SRGB);
        else
            std::cerr << "[E] failed to decode image " << in << ": " << stbi_failure_reason() << std::endl;
    }
    return SerializedImage();
}

static int format_size(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    case VK_FORMAT_R16G16B16A16_UINT:
        return 8;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return 4;
    default:
        assert(false);
        return 0;
    }
}

ImageGenerator::SerializedImage ImageGenerator::generate(void* raw_image_data, int width, int height, VkFormat input_format)
{
    size_t mip_count = 1 + floor(log2(std::max(width, height)));
    VkDescriptorPool descriptor_pool;
    VkDescriptorPoolSize descriptor_pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<uint32_t>(mip_count * 4) }
    };
    VkDescriptorPoolCreateInfo descriptor_pool_ci {};
    descriptor_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_ci.maxSets = mip_count * 2;
    descriptor_pool_ci.poolSizeCount = 1;
    descriptor_pool_ci.pPoolSizes = descriptor_pool_sizes;
    VK_DEMAND(vkCreateDescriptorPool(m_device, &descriptor_pool_ci, nullptr, &descriptor_pool));

    VkBuffer staging_buffer;
    VkDeviceMemory staging_buffer_mem;
    VkBufferCreateInfo buffer_ci {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = width * height * format_size(input_format) * 2;
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_DEMAND(vkCreateBuffer(m_device, &buffer_ci, nullptr, &staging_buffer));

    VkImage staging_image, storage_image, output_image;
    VkDeviceMemory staging_image_mem, storage_image_mem, output_image_mem;
    VkImageCreateInfo image_ci {};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = input_format;
    image_ci.extent.width = width;
    image_ci.extent.height = height;
    image_ci.extent.depth = 1;
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_DEMAND(vkCreateImage(m_device, &image_ci, nullptr, &staging_image));
    image_ci.mipLevels = mip_count;
    VK_DEMAND(vkCreateImage(m_device, &image_ci, nullptr, &output_image));
    image_ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    VK_DEMAND(vkCreateImage(m_device, &image_ci, nullptr, &storage_image));

    VkMemoryAllocateInfo alloc_info {};
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(m_device, staging_buffer, &mem_requirements);
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_DEMAND(vkAllocateMemory(m_device, &alloc_info, nullptr, &staging_buffer_mem));
    VK_DEMAND(vkBindBufferMemory(m_device, staging_buffer, staging_buffer_mem, 0));
    vkGetImageMemoryRequirements(m_device, staging_image, &mem_requirements);
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_DEMAND(vkAllocateMemory(m_device, &alloc_info, nullptr, &staging_image_mem));
    VK_DEMAND(vkBindImageMemory(m_device, staging_image, staging_image_mem, 0));
    vkGetImageMemoryRequirements(m_device, storage_image, &mem_requirements);
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_DEMAND(vkAllocateMemory(m_device, &alloc_info, nullptr, &storage_image_mem));
    VK_DEMAND(vkBindImageMemory(m_device, storage_image, storage_image_mem, 0));
    vkGetImageMemoryRequirements(m_device, output_image, &mem_requirements);
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_DEMAND(vkAllocateMemory(m_device, &alloc_info, nullptr, &output_image_mem));
    VK_DEMAND(vkBindImageMemory(m_device, output_image, output_image_mem, 0));

    std::vector<VkImageView> image_views(mip_count);
    VkImageViewCreateInfo image_view_ci {};
    image_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_ci.image = storage_image;
    image_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_ci.format = image_ci.format;
    image_view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_view_ci.subresourceRange.levelCount = 1;
    image_view_ci.subresourceRange.baseArrayLayer = 0;
    image_view_ci.subresourceRange.layerCount = 1;
    for (size_t i = 0; i < mip_count; i++) {
        image_view_ci.subresourceRange.baseMipLevel = i;
        VK_DEMAND(vkCreateImageView(m_device, &image_view_ci, nullptr, &image_views[i]));
    }

    void* sbdata;
    VK_DEMAND(vkMapMemory(m_device, staging_buffer_mem, 0, VK_WHOLE_SIZE, 0, &sbdata));
    memcpy(sbdata, raw_image_data, width * height * format_size(input_format));
    vkUnmapMemory(m_device, staging_buffer_mem);
    STBI_FREE(raw_image_data);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cb_info {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandPool = m_command_pool;
    cb_info.commandBufferCount = 1;
    VK_DEMAND(vkAllocateCommandBuffers(m_device, &cb_info, &cmd));

    std::vector<VkDescriptorSet> descriptor_sets(mip_count - 1);
    std::vector<VkDescriptorSetLayout> descriptor_alloc_layouts(descriptor_sets.size());
    std::vector<VkWriteDescriptorSet> descriptor_writes(descriptor_sets.size() * 2);
    std::vector<VkDescriptorImageInfo> descriptor_images(descriptor_sets.size() * 2);
    VkDescriptorSetAllocateInfo descriptor_alloc_info {};
    descriptor_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_alloc_info.descriptorPool = descriptor_pool;
    descriptor_alloc_info.descriptorSetCount = descriptor_sets.size();
    descriptor_alloc_info.pSetLayouts = descriptor_alloc_layouts.data();
    std::fill(descriptor_alloc_layouts.begin(), descriptor_alloc_layouts.end(), m_descriptor_layout);
    vkAllocateDescriptorSets(m_device, &descriptor_alloc_info, descriptor_sets.data());

#ifdef LINK_RENDERDOC
    if (m_debugger)
        reinterpret_cast<RENDERDOC_API_1_1_2*>(m_debugger)->StartFrameCapture(nullptr, nullptr);
#endif

    for (size_t i = 0; i < descriptor_writes.size(); i++) {
        descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[i].pNext = nullptr;
        descriptor_writes[i].dstSet = descriptor_sets[i / 2];
        descriptor_writes[i].dstBinding = i % 2;
        descriptor_writes[i].descriptorCount = 1;
        descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptor_writes[i].pImageInfo = &descriptor_images[i];
        descriptor_images[i].sampler = VK_NULL_HANDLE;
        descriptor_images[i].imageView = image_views[(i + 1) / 2];
        descriptor_images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkUpdateDescriptorSets(m_device, descriptor_writes.size(), descriptor_writes.data(), 0, nullptr);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = staging_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    barrier.image = storage_image;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    barrier.image = output_image;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    // Ensure that for {barrier.image}, image layouts are changed, and
    // {0} operations during {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT} is all done
    // before any {VK_ACCESS_TRANSFER_WRITE_BIT} operations during {VK_PIPELINE_STAGE_TRANSFER_BIT}

    VkBufferImageCopy bcopy {};
    bcopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bcopy.imageSubresource.mipLevel = 0;
    bcopy.imageSubresource.baseArrayLayer = 0;
    bcopy.imageSubresource.layerCount = 1;
    bcopy.imageExtent = image_ci.extent;
    vkCmdCopyBufferToImage(cmd, staging_buffer, staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bcopy);

    barrier.image = staging_image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageBlit region {};
    region.srcOffsets[1].x = region.dstOffsets[1].x = width;
    region.srcOffsets[1].y = region.dstOffsets[1].y = height;
    region.srcOffsets[1].z = region.dstOffsets[1].z = 1;
    region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = region.dstSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = region.dstSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
    vkCmdBlitImage(cmd, staging_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storage_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    barrier.image = storage_image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

    size_t mip_width = width / 2, mip_height = height / 2;
    for (size_t i = 1; i < mip_count; i++) {
        if (i != 1) {
            barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = i;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &descriptor_sets[i - 1], 0, nullptr);
        vkCmdDispatch(cmd, (mip_width + 7) / 8, (mip_height + 7) / 8, 1);
        mip_width /= 2;
        mip_height /= 2;
    }

    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    std::vector<VkImageBlit> blitout(mip_count);
    for (size_t i = 0; i < mip_count; i++) {
        blitout[i].srcSubresource.aspectMask = blitout[i].dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitout[i].srcSubresource.mipLevel = blitout[i].dstSubresource.mipLevel = i;
        blitout[i].srcSubresource.baseArrayLayer = blitout[i].srcSubresource.baseArrayLayer = 0;
        blitout[i].srcSubresource.layerCount = blitout[i].dstSubresource.layerCount = 1;
        blitout[i].srcOffsets[1].x = blitout[i].dstOffsets[1].x = width >> i;
        blitout[i].srcOffsets[1].y = blitout[i].dstOffsets[1].y = height >> i;
        blitout[i].srcOffsets[1].z = blitout[i].dstOffsets[1].z = 1;
    }
    vkCmdBlitImage(cmd, storage_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blitout.size(), blitout.data(), VK_FILTER_NEAREST);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = output_image;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    size_t current_mip_offset = 0;
    std::vector<VkBufferImageCopy> writeout(mip_count);
    for (size_t i = 0; i < mip_count; i++) {
        writeout[i].bufferOffset = current_mip_offset;
        writeout[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        writeout[i].imageSubresource.mipLevel = i;
        writeout[i].imageSubresource.baseArrayLayer = 0;
        writeout[i].imageSubresource.layerCount = 1;
        writeout[i].bufferRowLength = writeout[i].bufferImageHeight = 0;
        writeout[i].imageExtent.width = width >> i;
        writeout[i].imageExtent.height = height >> i;
        writeout[i].imageExtent.depth = 1;
        current_mip_offset += (width >> i) * (height >> i) * format_size(input_format);
    }
    vkCmdCopyImageToBuffer(cmd, output_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer, writeout.size(), writeout.data());
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
    vkDeviceWaitIdle(m_device);

#ifdef LINK_RENDERDOC
    if (m_debugger)
        reinterpret_cast<RENDERDOC_API_1_1_2*>(m_debugger)->EndFrameCapture(nullptr, nullptr);
#endif

    ktxTexture2* out_handle;
    ktxTextureCreateInfo ktx_ci {};
    ktx_ci.vkFormat = input_format;
    ktx_ci.baseWidth = width;
    ktx_ci.baseHeight = height;
    ktx_ci.baseDepth = 1;
    ktx_ci.numDimensions = 2;
    ktx_ci.numLevels = mip_count;
    ktx_ci.numLayers = ktx_ci.numFaces = 1;
    assert(ktxTexture2_Create(&ktx_ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &out_handle) == KTX_SUCCESS);

    VK_DEMAND(vkMapMemory(m_device, staging_buffer_mem, 0, VK_WHOLE_SIZE, 0, &sbdata));
    for (size_t i = 0; i < mip_count; i++) {
        assert(ktxTexture_SetImageFromMemory(ktxTexture(out_handle), i, 0, 0,
                   reinterpret_cast<const uint8_t*>(sbdata) + writeout[i].bufferOffset,
                   (width >> i) * (height >> i) * format_size(input_format))
            == KTX_SUCCESS);
    }
    vkUnmapMemory(m_device, staging_buffer_mem);

    if (input_format == VK_FORMAT_R8G8B8A8_SRGB && m_enable_uastc) {
        ktxBasisParams basis {};
        ktx_error_code_e res;
        basis.structSize = sizeof(ktxBasisParams);
        basis.uastc = KTX_TRUE;
        basis.uastcRDO = KTX_TRUE;
        if ((res = ktxTexture2_CompressBasisEx(out_handle, &basis)) != KTX_SUCCESS) {
            std::cerr << "[C] could not encode texture to UASTC: " << res << std::endl;
            std::abort();
        }
    }

    ktx_size_t out_size;
    ktx_uint8_t* out_buffer;
    ktxTexture_WriteToMemory(ktxTexture(out_handle), &out_buffer, &out_size);

    SerializedImage ser(out_buffer, out_size);
    ktxTexture_Destroy(ktxTexture(out_handle));

    for (auto it = image_views.begin(); it != image_views.end(); ++it)
        vkDestroyImageView(m_device, *it, nullptr);
    vkDestroyImage(m_device, staging_image, nullptr);
    vkDestroyImage(m_device, storage_image, nullptr);
    vkDestroyImage(m_device, output_image, nullptr);
    vkDestroyBuffer(m_device, staging_buffer, nullptr);
    vkFreeMemory(m_device, staging_buffer_mem, nullptr);
    vkFreeMemory(m_device, staging_image_mem, nullptr);
    vkFreeMemory(m_device, storage_image_mem, nullptr);
    vkFreeMemory(m_device, output_image_mem, nullptr);
    vkDestroyDescriptorPool(m_device, descriptor_pool, nullptr);
    return ser;
}
