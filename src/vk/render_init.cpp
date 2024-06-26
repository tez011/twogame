#define VK_ENABLE_BETA_EXTENSIONS
#include <algorithm>
#include <csignal>
#include <exception>
#include <set>
#include <string>
#include <cglm/cglm.h>
#include <cglm/clipspace/persp_lh_zo.h>
#include <physfs.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include "render.h"
#include "twogame.h"

#if TWOGAME_DEBUG_BUILD && !defined(__APPLE__)
constexpr static bool ENABLE_VALIDATION_LAYERS = true;
constexpr static const char* INSTANCE_LAYERS[] = { "VK_LAYER_KHRONOS_validation" };
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 1;
#else
constexpr static bool ENABLE_VALIDATION_LAYERS = false;
constexpr static const char** INSTANCE_LAYERS = nullptr;
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 0;
#endif
constexpr static const char* PIPELINE_CACHE_NAME = "/pref/shader-cache.vk.plc";

namespace twogame {

PFN_vkDestroyDebugUtilsMessengerEXT Renderer::s_vkDestroyDebugUtilsMessenger = nullptr;

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* cb_data, void* user_data)
{
    spdlog::level::level_enum which_level;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        which_level = spdlog::level::err;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        which_level = spdlog::level::warn;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT && ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0))
        which_level = spdlog::level::info;
    else
        which_level = spdlog::level::debug;

    spdlog::log(which_level, "Vulkan {}", cb_data->pMessage);

#ifdef TWOGAME_DEBUG_BUILD
    if (which_level == spdlog::level::err)
        std::raise(SIGABRT);
#endif

    return VK_FALSE;
}

Renderer::Renderer(Twogame* tg, SDL_Window* window)
    : m_twogame(tg)
    , m_window(window)
{
    create_instance();
    create_debug_messenger();
    if (SDL_Vulkan_CreateSurface(window, m_instance, &m_surface) == SDL_FALSE) {
        spdlog::critical("SDL_Vulkan_CreateSurface: {}\n", SDL_GetError());
        std::terminate();
    }

    pick_physical_device();
    create_logical_device();
    create_allocator();
    create_swapchain(VK_NULL_HANDLE);
    create_sampler();
    create_render_pass();
    create_framebuffers();
    create_pipeline_cache();
    create_descriptor_sets();
    create_command_buffers();
    create_synchronizers();
}

Renderer::~Renderer()
{
    for (size_t i = 0; i < 2; i++) {
        for (auto it = m_command_pools[i].begin(); it != m_command_pools[i].end(); ++it) {
            for (auto jt = it->begin(); jt != it->end(); ++jt)
                vkDestroyCommandPool(m_device, *jt, nullptr);
        }
        vkDestroySemaphore(m_device, m_sem_image_available[i], nullptr);
        vkDestroySemaphore(m_device, m_sem_render_finished[i], nullptr);
        vkDestroySemaphore(m_device, m_sem_blit_finished[i], nullptr);
        vkDestroyFence(m_device, m_fence_frame[i], nullptr);

        vkDestroyDescriptorPool(m_device, m_ds01_pool[i], nullptr);
    }
    for (size_t i = 0; i < DS1_INSTANCES; i++)
        vmaDestroyBuffer(m_allocator, m_ds1_buffers[i].buffer, m_ds1_buffers[i].allocation);
    for (size_t i = 0; i < DS2_BUFFERS; i++)
        delete m_ds2_buffer_pool[i];
    delete m_ds2_pool;
    if (m_dummy_image) {
        vkDestroyImageView(m_device, m_dummy_image_view_2d, nullptr);
        vmaDestroyImage(m_allocator, m_dummy_image, m_dummy_image_allocation);
    }

    vkDestroyCommandPool(m_device, m_command_pool_transient, nullptr);
    vkDestroyFence(m_device, m_fence_assets_prepared, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);

    for (size_t i = 0; i < 2; i++) {
        defer_free(m_framebuffers[i]);
        defer_free(m_render_att_views[i]);
        defer_free(m_render_atts[i]);
        defer_free(m_render_att_allocs[i]);
    }
    for (size_t i = 0; i < m_trash.size(); i++)
        release_freed_items(i);
    vkDestroyDescriptorSetLayout(m_device, m_ds0_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_ds1_layout, nullptr);

    vkDestroySampler(m_device, m_active_sampler, nullptr);
    vkDestroySampler(m_device, m_morph_sampler, nullptr);
    vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
    vkDestroyRenderPass(m_device, m_render_pass[0], nullptr);
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debug_messenger != VK_NULL_HANDLE)
        s_vkDestroyDebugUtilsMessenger(m_instance, m_debug_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

void Renderer::create_instance()
{
    unsigned int n;
    std::vector<const char*> instance_extensions;
    if (SDL_Vulkan_GetInstanceExtensions(m_window, &n, nullptr) == SDL_FALSE) {
        spdlog::critical("SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
        std::terminate();
    }
    instance_extensions.resize(n);
    if (SDL_Vulkan_GetInstanceExtensions(m_window, &n, instance_extensions.data()) == SDL_FALSE) {
        spdlog::critical("SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
        std::terminate();
    }
    if (ENABLE_VALIDATION_LAYERS)
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags instance_create_flags = 0;
    std::vector<VkExtensionProperties> available_instance_extensions;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr));
    available_instance_extensions.resize(n);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &n, available_instance_extensions.data()));
    for (auto& ext : available_instance_extensions) {
        if (strcmp(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, ext.extensionName) == 0) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }

    VkApplicationInfo appinfo {};
    appinfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appinfo.apiVersion = API_VERSION;

    VkInstanceCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createinfo.flags = instance_create_flags;
    createinfo.pApplicationInfo = &appinfo;
    createinfo.enabledLayerCount = INSTANCE_LAYERS_COUNT;
    createinfo.ppEnabledLayerNames = INSTANCE_LAYERS;
    createinfo.enabledExtensionCount = instance_extensions.size();
    createinfo.ppEnabledExtensionNames = instance_extensions.data();
    VK_CHECK(vkCreateInstance(&createinfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);
}

void Renderer::create_debug_messenger()
{
    if (ENABLE_VALIDATION_LAYERS == false)
        return;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    auto vkCreateDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (s_vkDestroyDebugUtilsMessenger == nullptr)
        s_vkDestroyDebugUtilsMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkCreateDebugUtilsMessenger == nullptr || s_vkDestroyDebugUtilsMessenger == nullptr) {
        spdlog::critical("vulkan extension " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " not present");
        std::terminate();
    }

    VkDebugUtilsMessengerCreateInfoEXT createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createinfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createinfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createinfo.pfnUserCallback = vk_debug_callback;

    VK_CHECK(vkCreateDebugUtilsMessenger(m_instance, &createinfo, nullptr, &m_debug_messenger));
}

static bool evaluate_physical_device(VkPhysicalDevice hwd, VkSurfaceKHR surface, const VkPhysicalDeviceProperties& device_props)
{
    uint32_t count;
    std::vector<VkQueueFamilyProperties> qfprop;
    vkGetPhysicalDeviceQueueFamilyProperties(hwd, &count, nullptr);
    qfprop.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(hwd, &count, qfprop.data());
    bool has_good_queue = false;
    for (uint32_t i = 0; i < count; i++) {
        const auto& queue_family = qfprop[i];
        if ((queue_family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            VkBool32 present_support = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(hwd, i, surface, &present_support);
            if (present_support && qfprop[i].queueCount > 0)
                has_good_queue = true;
        }
    }
    if (has_good_queue == false) {
        spdlog::debug("{}: skipping: no queue family supports graphics, compute, and presentation", device_props.deviceName);
        return false;
    }

    std::vector<VkExtensionProperties> available_exts;
    bool has_portability_subset = false;
    vkEnumerateDeviceExtensionProperties(hwd, nullptr, &count, nullptr);
    available_exts.resize(count);
    vkEnumerateDeviceExtensionProperties(hwd, nullptr, &count, available_exts.data());
    std::set<std::string> required_exts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    for (const auto& ext : available_exts) {
        if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, ext.extensionName) == 0)
            has_portability_subset = true;
        auto it = required_exts.find(ext.extensionName);
        if (it != required_exts.end())
            required_exts.erase(it);
    }
    if (!required_exts.empty()) {
        for (const auto& missing_ext : required_exts)
            spdlog::debug("{}: skipping: missing required extension {}", device_props.deviceName, missing_ext.c_str());
        return false;
    }

    VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_features {};
    portability_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    VkPhysicalDeviceVulkan12Features available_features12 {};
    available_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    available_features12.pNext = has_portability_subset ? &portability_features : nullptr;
    VkPhysicalDeviceVulkan11Features available_features11 {};
    available_features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    available_features11.pNext = &available_features12;
    VkPhysicalDeviceFeatures2 available_features {};
    available_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    available_features.pNext = &available_features11;
    vkGetPhysicalDeviceFeatures2(hwd, &available_features);

#define DEMAND_FEATURE(STRUCTURE, FIELD)                                                                   \
    if (STRUCTURE.FIELD == VK_FALSE) {                                                                     \
        spdlog::debug("{}: skipping: required feature " #FIELD " not available", device_props.deviceName); \
        return false;                                                                                      \
    }
    DEMAND_FEATURE(available_features.features, depthClamp);
    DEMAND_FEATURE(available_features.features, sampleRateShading);
    if (has_portability_subset) {
        DEMAND_FEATURE(portability_features, constantAlphaColorBlendFactors);
        DEMAND_FEATURE(portability_features, events);
    }
#undef DEMAND_FEATURE

    uint32_t surface_format_count, surface_present_mode_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(hwd, surface, &surface_format_count, nullptr);
    if (surface_format_count == 0) {
        spdlog::debug("{}: skipping: no supported surface formats", device_props.deviceName);
        return false;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(hwd, surface, &surface_present_mode_count, nullptr);
    if (surface_present_mode_count == 0) {
        spdlog::debug("{}: skipping: no supported surface present modes", device_props.deviceName);
        return false;
    }

    VkImageFormatProperties ifmt;
    if (vkGetPhysicalDeviceImageFormatProperties(hwd, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ifmt) == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        spdlog::debug("{}: skipping: image format used for morph target displacements is not supported", device_props.deviceName);
        return false;
    }

    return true;
}

void Renderer::pick_physical_device()
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
        if (evaluate_physical_device(device, m_surface, device_props)) {
            m_hwd = device;
            if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                dGPU = device;
            } else if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                iGPU = device;
            }
        }
    }

    // m_hwd is the last device that is barely usable; dGPU is the first discrete GPU; iGPU is the first integrated GPU.
    if (dGPU != VK_NULL_HANDLE)
        m_hwd = dGPU;
    else if (iGPU != VK_NULL_HANDLE)
        m_hwd = iGPU;
}

constexpr static float s_queue_priorities[] = { 1.0f, 0.5f };
void Renderer::create_logical_device()
{
    uint32_t count = 0;
    std::vector<VkExtensionProperties> available_exts;
    std::vector<const char*> extensions;
    if (m_hwd == VK_NULL_HANDLE) {
        spdlog::critical("no usable physical devices were found");
        std::terminate();
    }

    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, nullptr);
    available_exts.resize(count);
    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, available_exts.data());

    for (auto& ext : available_exts) {
        if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (strcmp(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        if (strcmp(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        if (strcmp(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, ext.extensionName) == 0) {
            extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
            extensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
        }
    }

    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(m_hwd, &properties);
    spdlog::info("selecting device {}", properties.properties.deviceName);
#ifdef TWOGAME_DEBUG_BUILD
    for (auto& e : extensions)
        spdlog::info("    with {}", e);
#endif

    // Enable all features that are available.
    VkPhysicalDeviceFeatures2 device_features {};
    VkPhysicalDeviceVulkan11Features device_features11 {};
    VkPhysicalDeviceVulkan12Features device_features12 {};
    VkPhysicalDeviceRobustness2FeaturesEXT device_features_robustness2 {};
    device_features_robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    device_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    device_features12.pNext = &device_features_robustness2;
    device_features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    device_features11.pNext = &device_features12;
    device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device_features.pNext = &device_features11;
    vkGetPhysicalDeviceFeatures2(m_hwd, &device_features);
    memcpy(&m_device_limits, &properties.properties.limits, sizeof(VkPhysicalDeviceLimits));

    std::array<VkDeviceQueueCreateInfo, 3> queue_createinfos {};
    std::vector<VkQueueFamilyProperties> qf_properties;
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, nullptr);
    qf_properties.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, qf_properties.data());

    uint32_t qf_primary = UINT32_MAX, qf_compute = UINT32_MAX, qf_transfer = UINT32_MAX;
    VkDeviceSize transfer_granularity = 4000000000UL;
    for (uint32_t i = 0; i < count; i++) {
        if ((qf_properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_hwd, i, m_surface, &present_support);
            if (qf_primary == UINT32_MAX && present_support)
                qf_primary = i;
        }
        if ((qf_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            if (qf_compute == UINT32_MAX && qf_properties[i].queueCount > 0)
                qf_compute = i;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((qf_properties[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && i != qf_primary && i != qf_compute) {
            VkDeviceSize gr = qf_properties[i].minImageTransferGranularity.width;
            gr *= qf_properties[i].minImageTransferGranularity.height;
            gr *= qf_properties[i].minImageTransferGranularity.depth;
            if (gr < transfer_granularity && qf_properties[i].queueCount > 0) {
                transfer_granularity = gr;
                qf_transfer = i;
            }
        }
    }

    int queue_family_count = 0;
    queue_createinfos[queue_family_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_createinfos[queue_family_count].queueFamilyIndex = qf_primary;
    queue_createinfos[queue_family_count].queueCount = 1;
    queue_createinfos[queue_family_count].pQueuePriorities = s_queue_priorities;
    if (qf_compute != UINT32_MAX) {
        queue_createinfos[++queue_family_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_createinfos[queue_family_count].queueFamilyIndex = qf_compute;
        queue_createinfos[queue_family_count].queueCount = 1;
        queue_createinfos[queue_family_count].pQueuePriorities = s_queue_priorities;
    }
    if (qf_transfer != UINT32_MAX) {
        queue_createinfos[++queue_family_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_createinfos[queue_family_count].queueFamilyIndex = qf_transfer;
        queue_createinfos[queue_family_count].queueCount = 1;
        queue_createinfos[queue_family_count].pQueuePriorities = s_queue_priorities;
    }

    VkDeviceCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createinfo.pNext = &device_features;
    createinfo.queueCreateInfoCount = ++queue_family_count;
    createinfo.pQueueCreateInfos = queue_createinfos.data();
    createinfo.enabledExtensionCount = extensions.size();
    createinfo.ppEnabledExtensionNames = extensions.data();
    createinfo.pEnabledFeatures = nullptr;
    VK_CHECK(vkCreateDevice(m_hwd, &createinfo, nullptr, &m_device));
    volkLoadDevice(m_device);

    m_queue_families[static_cast<size_t>(QueueFamily::Universal)] = qf_primary;
    m_queue_families[static_cast<size_t>(QueueFamily::Compute)] = qf_compute;
    m_queue_families[static_cast<size_t>(QueueFamily::Transfer)] = qf_transfer;
    vkGetDeviceQueue(m_device, qf_primary, 0, &m_queues[static_cast<size_t>(QueueFamily::Universal)]);
    if (qf_compute != UINT32_MAX)
        vkGetDeviceQueue(m_device, qf_compute, 0, &m_queues[static_cast<size_t>(QueueFamily::Compute)]);
    if (qf_transfer != UINT32_MAX)
        vkGetDeviceQueue(m_device, qf_transfer, 0, &m_queues[static_cast<size_t>(QueueFamily::Transfer)]);
}

void Renderer::create_allocator()
{
    VmaAllocatorCreateInfo createinfo {};
    VmaVulkanFunctions vfn {};
    createinfo.flags = 0;
    createinfo.physicalDevice = m_hwd;
    createinfo.device = m_device;
    createinfo.instance = m_instance;
    createinfo.vulkanApiVersion = API_VERSION;
    createinfo.pVulkanFunctions = &vfn;
    vfn.vkGetInstanceProcAddr = vkGetInstanceProcAddr; // provided by volk loader
    vfn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VK_CHECK(vmaCreateAllocator(&createinfo, &m_allocator));
}

void Renderer::create_swapchain(VkSwapchainKHR old_swapchain)
{
    VkSurfaceCapabilitiesKHR capabilities;
    uint32_t n = 0;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_hwd, m_surface, &capabilities);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_hwd, m_surface, &n, nullptr);
    formats.resize(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_hwd, m_surface, &n, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_hwd, m_surface, &n, nullptr);
    present_modes.resize(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_hwd, m_surface, &n, present_modes.data());

    if (std::any_of(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& fmt) {
            return fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        })) {
        m_swapchain_format.format = VK_FORMAT_B8G8R8A8_SRGB;
        m_swapchain_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
        m_swapchain_format = formats[0];
    }

    if (capabilities.currentExtent.width == std::numeric_limits<uint32_t>::max()) {
        int w, h;
        SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
        m_swapchain_extent.width = std::clamp(static_cast<uint32_t>(w), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        m_swapchain_extent.height = std::clamp(static_cast<uint32_t>(h), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    } else {
        m_swapchain_extent = capabilities.currentExtent;
    }

    float aspect = static_cast<float>(m_swapchain_extent.width) / static_cast<float>(m_swapchain_extent.height);
    uint32_t image_count = capabilities.minImageCount + 2;
    if (capabilities.maxImageCount > 0)
        image_count = std::min(image_count, capabilities.maxImageCount);

    VkSwapchainCreateInfoKHR createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createinfo.surface = m_surface;
    createinfo.minImageCount = image_count;
    createinfo.imageFormat = m_swapchain_format.format;
    createinfo.imageColorSpace = m_swapchain_format.colorSpace;
    createinfo.imageExtent = m_swapchain_extent;
    createinfo.imageArrayLayers = 1;
    createinfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createinfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createinfo.preTransform = capabilities.currentTransform;
    createinfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createinfo.clipped = VK_TRUE;
    createinfo.oldSwapchain = old_swapchain;
    if (std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != present_modes.end())
        createinfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    else
        createinfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VK_CHECK(vkCreateSwapchainKHR(m_device, &createinfo, nullptr, &m_swapchain));

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data());
    spdlog::debug("obtained {} swapchain images", image_count);

    glm_mat4_zero(m_projection);
    m_projection[0][0] = m_cot_vertical_fov / aspect;
    m_projection[1][1] = -m_cot_vertical_fov;
    m_projection[2][2] = -1.0f;
    m_projection[2][3] = -1.0f;
    m_projection[3][2] = -0.1f;
}

void Renderer::create_render_pass()
{
    auto depth_candidates = std::to_array<VkFormat>({ VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT });
    for (size_t i = 0; i < depth_candidates.size() && m_depth_format == VK_FORMAT_UNDEFINED; i++) {
        VkImageFormatProperties props {};
        VkResult res = vkGetPhysicalDeviceImageFormatProperties(m_hwd, depth_candidates[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &props);
        if (res == VK_SUCCESS)
            m_depth_format = depth_candidates[i];
        else if (res != VK_ERROR_FORMAT_NOT_SUPPORTED)
            VK_CHECK(res);
    }
    if (m_depth_format == VK_FORMAT_UNDEFINED) {
        spdlog::critical("no usable depth/stencil formats were found");
        std::terminate();
    }

    VkRenderPassCreateInfo createinfo {};
    std::array<VkAttachmentDescription, 2> attachments = {};
    std::array<VkAttachmentReference, 1> refs = {};
    std::array<VkSubpassDescription, 1> subpasses = {};
    std::array<VkSubpassDependency, 2> deps = {};
    VkAttachmentReference depth_ref {};

    attachments[0].format = m_swapchain_format.format;
    attachments[0].samples = static_cast<VkSampleCountFlagBits>(m_multisample_count);
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = m_depth_format;
    attachments[1].samples = static_cast<VkSampleCountFlagBits>(m_multisample_count);
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    refs[0].attachment = 0;
    refs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = refs.size();
    subpasses[0].pColorAttachments = refs.data();
    subpasses[0].pDepthStencilAttachment = &depth_ref;

    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].dstSubpass = 0;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    createinfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createinfo.attachmentCount = attachments.size();
    createinfo.pAttachments = attachments.data();
    createinfo.subpassCount = subpasses.size();
    createinfo.pSubpasses = subpasses.data();
    createinfo.dependencyCount = deps.size();
    createinfo.pDependencies = deps.data();
    VK_CHECK(vkCreateRenderPass(m_device, &createinfo, nullptr, &m_render_pass[0]));
}

void Renderer::create_framebuffers()
{
    constexpr size_t n_attachments = static_cast<size_t>(RenderAttachment::MAX_VALUE);
    VkImageCreateInfo i_createinfo {};
    VkImageViewCreateInfo v_createinfo {};
    VmaAllocationCreateInfo a_createinfo {};
    a_createinfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    a_createinfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    a_createinfo.priority = 1.f;

    i_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i_createinfo.imageType = VK_IMAGE_TYPE_2D;
    i_createinfo.format = m_swapchain_format.format;
    i_createinfo.extent.width = m_swapchain_extent.width;
    i_createinfo.extent.height = m_swapchain_extent.height;
    i_createinfo.extent.depth = 1;
    i_createinfo.mipLevels = 1;
    i_createinfo.arrayLayers = 1;
    i_createinfo.samples = VK_SAMPLE_COUNT_1_BIT;
    i_createinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    i_createinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    i_createinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    v_createinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v_createinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v_createinfo.format = i_createinfo.format;
    v_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v_createinfo.subresourceRange.baseMipLevel = 0;
    v_createinfo.subresourceRange.levelCount = 1;
    v_createinfo.subresourceRange.baseArrayLayer = 0;
    v_createinfo.subresourceRange.layerCount = 1;
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vmaCreateImage(m_allocator, &i_createinfo, &a_createinfo,
            &m_render_atts[i][static_cast<size_t>(RenderAttachment::ColorBuffer)],
            &m_render_att_allocs[i][static_cast<size_t>(RenderAttachment::ColorBuffer)],
            nullptr));
        v_createinfo.image = m_render_atts[i][static_cast<size_t>(RenderAttachment::ColorBuffer)];
        VK_CHECK(vkCreateImageView(m_device, &v_createinfo, nullptr,
            &m_render_att_views[i][static_cast<size_t>(RenderAttachment::ColorBuffer)]));
    }

    i_createinfo.format = m_depth_format;
    i_createinfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    v_createinfo.format = i_createinfo.format;
    v_createinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vmaCreateImage(m_allocator, &i_createinfo, &a_createinfo,
            &m_render_atts[i][static_cast<size_t>(RenderAttachment::DepthBuffer)],
            &m_render_att_allocs[i][static_cast<size_t>(RenderAttachment::DepthBuffer)],
            nullptr));
        v_createinfo.image = m_render_atts[i][static_cast<size_t>(RenderAttachment::DepthBuffer)];
        VK_CHECK(vkCreateImageView(m_device, &v_createinfo, nullptr,
            &m_render_att_views[i][static_cast<size_t>(RenderAttachment::DepthBuffer)]));
    }

    VkFramebufferCreateInfo fb_createinfo {};
    VkImageView fb_attachments[n_attachments];
    fb_createinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_createinfo.renderPass = m_render_pass[0];
    fb_createinfo.attachmentCount = 2;
    fb_createinfo.pAttachments = fb_attachments;
    fb_createinfo.width = m_swapchain_extent.width;
    fb_createinfo.height = m_swapchain_extent.height;
    fb_createinfo.layers = 1;
    for (int i = 0; i < 2; i++) {
        fb_attachments[0] = m_render_att_views[i][static_cast<size_t>(RenderAttachment::ColorBuffer)];
        fb_attachments[1] = m_render_att_views[i][static_cast<size_t>(RenderAttachment::DepthBuffer)];
        VK_CHECK(vkCreateFramebuffer(m_device, &fb_createinfo, nullptr, &m_framebuffers[i][0]));
    }
}

void Renderer::create_pipeline_cache()
{
    VkPipelineCacheCreateInfo cache_ci {};
    char* cidata = nullptr;
    cache_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    PHYSFS_Stat pcs;
    if (PHYSFS_stat(PIPELINE_CACHE_NAME, &pcs) != 0 && pcs.filetype == PHYSFS_FILETYPE_REGULAR && pcs.filesize > 0) {
        PHYSFS_File* fh = PHYSFS_openRead(PIPELINE_CACHE_NAME);
        cidata = new char[pcs.filesize];

        auto rc = PHYSFS_readBytes(fh, cidata, pcs.filesize);
        if (rc >= 0) {
            cache_ci.initialDataSize = rc;
            cache_ci.pInitialData = cidata;
        }
        PHYSFS_close(fh);
    }

    VK_CHECK(vkCreatePipelineCache(m_device, &cache_ci, nullptr, &m_pipeline_cache));
    delete[] cidata;
}

void Renderer::create_descriptor_sets()
{
    VkDescriptorSetLayoutBinding internal_bindings[] = {
        { static_cast<size_t>(DescriptorSetSlot::ProjectionView), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { static_cast<size_t>(DescriptorSetSlot::ModelMatrix), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { static_cast<size_t>(DescriptorSetSlot::BoneMatrices), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { static_cast<size_t>(DescriptorSetSlot::ShapeKeyWeights), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
        { static_cast<size_t>(DescriptorSetSlot::PositionDisplacements), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, &m_morph_sampler },
        { static_cast<size_t>(DescriptorSetSlot::NormalDisplacements), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, &m_morph_sampler },
    };
    m_push_constant_layout[0] = { VK_SHADER_STAGE_VERTEX_BIT, 0, 4 };

    VkDescriptorSetLayoutCreateInfo dsl_ci {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 0;
    dsl_ci.pBindings = internal_bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &dsl_ci, nullptr, &m_ds0_layout));
    dsl_ci.pBindings += dsl_ci.bindingCount;
    dsl_ci.bindingCount = 1;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &dsl_ci, nullptr, &m_ds1_layout));
    dsl_ci.pBindings += dsl_ci.bindingCount;
    dsl_ci.bindingCount = 5;
    m_ds2_pool = new vk::DescriptorPool(*this, dsl_ci);

    auto dsp01sz = std::to_array<VkDescriptorPoolSize>({
        // descriptor set 0
        // descriptor set 1
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, DS1_INSTANCES },
    });
    VkDescriptorPoolCreateInfo dpl_ci {};
    dpl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpl_ci.maxSets = 1 + DS1_INSTANCES;
    dpl_ci.poolSizeCount = dsp01sz.size();
    dpl_ci.pPoolSizes = dsp01sz.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &dpl_ci, nullptr, &m_ds01_pool[0]));
    VK_CHECK(vkCreateDescriptorPool(m_device, &dpl_ci, nullptr, &m_ds01_pool[1]));

    std::array<VkDescriptorSetLayout, DS1_INSTANCES> dslp;
    VkDescriptorSetAllocateInfo dsallocinfo {};
    dsallocinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    for (int i = 0; i < 2; i++) {
        dsallocinfo.descriptorPool = m_ds01_pool[i];
        dsallocinfo.descriptorSetCount = 1;
        dsallocinfo.pSetLayouts = &m_ds0_layout;
        VK_CHECK(vkAllocateDescriptorSets(m_device, &dsallocinfo, &m_ds0[i]));

        dslp.fill(m_ds1_layout);
        dsallocinfo.descriptorSetCount = DS1_INSTANCES;
        dsallocinfo.pSetLayouts = dslp.data();
        VK_CHECK(vkAllocateDescriptorSets(m_device, &dsallocinfo, m_ds1[i].data()));
    }

    std::array<VkDescriptorSetLayout, 3> plci_layout = { m_ds0_layout, m_ds1_layout, m_ds2_pool->layout() };
    VkPipelineLayoutCreateInfo plci {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = plci_layout.size();
    plci.pSetLayouts = plci_layout.data();
    plci.pushConstantRangeCount = m_push_constant_layout.size();
    plci.pPushConstantRanges = m_push_constant_layout.data();
    VK_CHECK(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeline_layout));

    VkBufferCreateInfo buffer_ci {};
    VmaAllocationCreateInfo buffer_ai {};
    std::vector<VkWriteDescriptorSet> writes;
    std::list<VkDescriptorBufferInfo> w_buffers;
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = sizeof(descriptor_storage::uniform_s1i0_t) * 2;
    buffer_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    buffer_ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK_CHECK(vmaCreateBuffer(m_allocator, &buffer_ci, &buffer_ai, &m_ds1_buffers[0].buffer, &m_ds1_buffers[0].allocation, &m_ds1_buffers[0].details));
    for (int i = 0; i < 2; i++) {
        auto& dsw = writes.emplace_back();
        auto& dsb = w_buffers.emplace_back();
        dsw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dsw.dstSet = m_ds1[i][0];
        dsw.dstBinding = 0;
        dsw.dstArrayElement = 0;
        dsw.descriptorCount = 1;
        dsw.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsw.pBufferInfo = &dsb;
        dsb.buffer = m_ds1_buffers[0].buffer;
        dsb.offset = i * sizeof(descriptor_storage::uniform_s1i0_t);
        dsb.range = sizeof(descriptor_storage::uniform_s1i0_t);
    }
    vkUpdateDescriptorSets(m_device, writes.size(), writes.data(), 0, nullptr);

    m_ds2_buffer_pool[static_cast<size_t>(DescriptorSetSlot::ModelMatrix)] = new vk::BufferPool(*this, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(mat4), 1024);
    m_ds2_buffer_pool[static_cast<size_t>(DescriptorSetSlot::BoneMatrices)] = new vk::BufferPool(*this, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 32 * sizeof(mat4), 1024);
    m_ds2_buffer_pool[static_cast<size_t>(DescriptorSetSlot::ShapeKeyWeights)] = new vk::BufferPool(*this, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 64 * sizeof(float), 1024);

    VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features {};
    robustness2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &robustness2_features;
    vkGetPhysicalDeviceFeatures2(m_hwd, &features);
    if (robustness2_features.nullDescriptor) {
        m_dummy_image = VK_NULL_HANDLE;
        m_dummy_image_view_2d = VK_NULL_HANDLE;
    } else {
        VmaAllocationCreateInfo alloc_ci {};
        VkImageCreateInfo image_ci {};
        image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_ci.imageType = VK_IMAGE_TYPE_2D;
        image_ci.format = VK_FORMAT_R8G8B8A8_UINT;
        image_ci.extent.width = image_ci.extent.height = image_ci.extent.depth = 1;
        image_ci.mipLevels = 1;
        image_ci.arrayLayers = 1;
        image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VK_CHECK(vmaCreateImage(m_allocator, &image_ci, &alloc_ci, &m_dummy_image, &m_dummy_image_allocation, nullptr));

        VkImageViewCreateInfo view_ci {};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = m_dummy_image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        view_ci.subresourceRange.baseMipLevel = 0;
        view_ci.subresourceRange.levelCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &view_ci, nullptr, &m_dummy_image_view_2d));
    }
}

void Renderer::create_command_buffers()
{
    VkCommandPoolCreateInfo pool_ci {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    for (int i = 0; i < 2; i++) {
        m_command_pools[i][static_cast<size_t>(QueueFamily::Universal)].resize(m_twogame->thread_pool().thread_count());
        pool_ci.queueFamilyIndex = m_queue_families[static_cast<size_t>(QueueFamily::Universal)];
        for (size_t j = 0; j < m_twogame->thread_pool().thread_count(); j++) {
            VK_CHECK(vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_command_pools[i][static_cast<size_t>(QueueFamily::Universal)][j]));
        }

        m_command_pools[i][static_cast<size_t>(QueueFamily::Compute)].resize(m_twogame->thread_pool().thread_count());
        pool_ci.queueFamilyIndex = m_queue_families[static_cast<size_t>(QueueFamily::Compute)];
        for (size_t j = 0; pool_ci.queueFamilyIndex != UINT32_MAX && j < m_twogame->thread_pool().thread_count(); j++) {
            VK_CHECK(vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_command_pools[i][static_cast<size_t>(QueueFamily::Compute)][j]));
        }
    }

    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = m_queue_families[static_cast<size_t>(QueueFamily::Universal)];
    VK_CHECK(vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_command_pool_transient));

    VkCommandBufferAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    for (int i = 0; i < 2; i++) {
        alloc_info.commandPool = m_command_pools[i][static_cast<size_t>(QueueFamily::Universal)][0];
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 2;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &alloc_info, &m_command_buffers[i][0]));
    }

    alloc_info.commandPool = m_command_pool_transient;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &alloc_info, &m_cbuf_asset_prepare));
}

void Renderer::create_synchronizers()
{
    VkSemaphoreCreateInfo sem_ci {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_image_available[i]));
        VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_render_finished[i]));
        VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_blit_finished[i]));
    }

    VkFenceCreateInfo fence_ci {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(m_device, &fence_ci, nullptr, &m_fence_assets_prepared));
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vkCreateFence(m_device, &fence_ci, nullptr, &m_fence_frame[i]));
    }
}

void Renderer::create_sampler()
{
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(m_hwd, &features);

    VkSamplerCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = m_mip_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (features.samplerAnisotropy && m_requested_anisotropy > 0) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(m_requested_anisotropy, m_device_limits.maxSamplerAnisotropy);
    }
    ci.minLod = 0;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_CHECK(vkCreateSampler(m_device, &ci, nullptr, &m_active_sampler));

    ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(m_device, &ci, nullptr, &m_morph_sampler));
}

void Renderer::release_freed_items(int bucket)
{
    decltype(m_trash)::value_type items;
    m_trash[bucket].swap(items);

    while (items.empty() == false) {
        uint64_t it = items.front().first;
        switch (items.front().second) {
        case vk_destructible::Types::DVkBuffer:
            vkDestroyBuffer(m_device, VkBuffer(it), nullptr);
            break;
        case vk_destructible::Types::DVkFramebuffer:
            vkDestroyFramebuffer(m_device, VkFramebuffer(it), nullptr);
            break;
        case vk_destructible::Types::DVkImage:
            vkDestroyImage(m_device, VkImage(it), nullptr);
            break;
        case vk_destructible::Types::DVkImageView:
            vkDestroyImageView(m_device, VkImageView(it), nullptr);
            break;
        case vk_destructible::Types::DVkRenderPass:
            vkDestroyRenderPass(m_device, VkRenderPass(it), nullptr);
            break;
        case vk_destructible::Types::DVkSampler:
            vkDestroySampler(m_device, VkSampler(it), nullptr);
            break;
        case vk_destructible::Types::DVkSwapchainKHR:
            vkDestroySwapchainKHR(m_device, VkSwapchainKHR(it), nullptr);
            break;
        case vk_destructible::Types::DVmaAllocation:
            vmaFreeMemory(m_allocator, VmaAllocation(it));
            break;
        default:
            std::terminate();
        }
        items.pop();
    }
}

void Renderer::recreate_swapchain()
{
    defer_free(m_swapchain);
    for (size_t i = 0; i < 2; i++) {
        defer_free(m_framebuffers[i]);
        defer_free(m_render_att_views[i]);
        defer_free(m_render_atts[i]);
        defer_free(m_render_att_allocs[i]);
    }

    VkSwapchainKHR old_swapchain = m_swapchain;
    vkDeviceWaitIdle(m_device);
    create_swapchain(old_swapchain);
    create_framebuffers();
}

void Renderer::write_pipeline_cache()
{
    static std::mutex pcmtx;
    size_t pcsz;
    VK_CHECK(vkGetPipelineCacheData(m_device, m_pipeline_cache, &pcsz, nullptr));

    std::vector<char> pcd(pcsz);
    VK_CHECK(vkGetPipelineCacheData(m_device, m_pipeline_cache, &pcsz, pcd.data()));

    {
        std::lock_guard<std::mutex> lk(pcmtx);
        PHYSFS_File* fh = PHYSFS_openWrite(PIPELINE_CACHE_NAME);
        PHYSFS_writeBytes(fh, pcd.data(), pcd.size());
        PHYSFS_close(fh);
    }
}

void Renderer::wait_idle()
{
    vkDeviceWaitIdle(m_device);
}

VkPipelineLayout Renderer::create_pipeline_layout(VkDescriptorSetLayout material_layout) const
{
    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo plci {};
    std::array<VkDescriptorSetLayout, 4> plci_layout = { m_ds0_layout, m_ds1_layout, m_ds2_pool->layout(), material_layout };
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = plci_layout.size();
    plci.pSetLayouts = plci_layout.data();
    plci.pushConstantRangeCount = m_push_constant_layout.size();
    plci.pPushConstantRanges = m_push_constant_layout.data();
    VK_CHECK(vkCreatePipelineLayout(m_device, &plci, nullptr, &layout));
    return layout;
}

VkPipeline Renderer::create_pipeline(VkGraphicsPipelineCreateInfo& pipeline_ci, size_t render_pass_index, size_t subpass_index) const
{
    VkPipeline pipeline;
    VkPipelineRasterizationStateCreateInfo rasterization_state {};
    VkPipelineMultisampleStateCreateInfo multisample_state {};
    rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization_state.lineWidth = 1.f;
    multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = static_cast<VkSampleCountFlagBits>(m_multisample_count),
    multisample_state.sampleShadingEnable = m_sample_shading > 0 ? VK_TRUE : VK_FALSE;
    multisample_state.minSampleShading = m_sample_shading;
    multisample_state.pSampleMask = nullptr;
    multisample_state.alphaToCoverageEnable = VK_FALSE;
    multisample_state.alphaToOneEnable = VK_FALSE;

    pipeline_ci.pRasterizationState = &rasterization_state;
    pipeline_ci.pMultisampleState = &multisample_state;
    pipeline_ci.renderPass = m_render_pass[render_pass_index];
    pipeline_ci.subpass = subpass_index;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, m_pipeline_cache, 1, &pipeline_ci, nullptr, &pipeline));
    return pipeline;
}

void Renderer::create_perobject_descriptors(std::array<VkDescriptorSet, 2>& sets, std::array<vk::BufferPool::index_t, DS2_BUFFERS * 2>& buffers)
{
    m_ds2_pool->allocate(sets.data(), sets.size());
    for (size_t i = 0; i < buffers.size(); i++)
        buffers[i] = m_ds2_buffer_pool[i / 2]->allocate();

    std::array<VkWriteDescriptorSet, DS2_BUFFERS * 2> writes;
    std::array<VkDescriptorBufferInfo, DS2_BUFFERS * 2> wbuffers;
    memset(writes.data(), 0, writes.size() * sizeof(VkWriteDescriptorSet));
    for (size_t i = 0; i < buffers.size(); i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = sets[i % 2];
        writes[i].dstBinding = i / 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &wbuffers[i];
        m_ds2_buffer_pool[i / 2]->buffer_handle(buffers[i], wbuffers[i]);
    }
    vkUpdateDescriptorSets(m_device, writes.size(), writes.data(), 0, nullptr);
}

void Renderer::free_perobject_descriptors(std::array<VkDescriptorSet, 2>& sets, std::array<vk::BufferPool::index_t, DS2_BUFFERS * 2>& buffers)
{
    for (size_t i = 0; i < buffers.size(); i++)
        m_ds2_buffer_pool[i / 2]->free(buffers[i]);
    m_ds2_pool->free(sets.data(), sets.size());
}

uint64_t Renderer::dummy_descriptor(DescriptorSetSlot s) const
{
    if (null_descriptor_enabled())
        return 0;

    switch (s) {
    case DescriptorSetSlot::PositionDisplacements:
    case DescriptorSetSlot::NormalDisplacements:
        return reinterpret_cast<uint64_t>(m_dummy_image_view_2d);
    default:
        return 0;
    }
}

}
