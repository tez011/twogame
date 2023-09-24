#define VK_ENABLE_BETA_EXTENSIONS
#include "vk.h"
#include <algorithm>
#include <csignal>
#include <exception>
#include <physfs.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <set>
#include <string>
#include <twogame.h>

#if TWOGAME_DEBUG_BUILD
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

PFN_vkDestroyDebugUtilsMessengerEXT VulkanRenderer::s_vkDestroyDebugUtilsMessenger = nullptr;

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* cb_data, void* user_data)
{
    SDL_LogPriority which_level;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        which_level = SDL_LOG_PRIORITY_ERROR;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        which_level = SDL_LOG_PRIORITY_WARN;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        which_level = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ? SDL_LOG_PRIORITY_INFO : SDL_LOG_PRIORITY_DEBUG;
    else
        which_level = SDL_LOG_PRIORITY_VERBOSE;

    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, which_level, "Vulkan %s", cb_data->pMessage);

#ifdef TWOGAME_DEBUG_BUILD
    if (which_level == SDL_LOG_PRIORITY_ERROR)
        std::raise(SIGABRT);
#endif

    return VK_FALSE;
}

VulkanRenderer::VulkanRenderer(Twogame* tg, SDL_Window* window)
    : Renderer(tg, window)
{
    create_instance();
    create_debug_messenger();
    if (SDL_Vulkan_CreateSurface(window, m_instance, &m_surface) == SDL_FALSE) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        std::terminate();
    }

    pick_physical_device();
    create_logical_device();
    create_allocator();
    create_swapchain(VK_NULL_HANDLE);
    create_render_pass();
    create_framebuffers();
    create_pipeline_cache();
    create_command_buffers();
    create_synchronizers();
    create_sampler();
}

VulkanRenderer::~VulkanRenderer()
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
    }
    for (size_t i = 0; i < 2; i++) {
        vfree(m_framebuffers[i]);
        vfree(m_render_att_views[i]);
        vfree(m_render_atts[i]);
        vfree(m_render_att_allocs[i]);
    }
    for (size_t i = 0; i < m_trash.size(); i++)
        release_freed_items(i);

    vkDestroySampler(m_device, m_active_sampler, nullptr);
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

void VulkanRenderer::create_instance()
{
    VkResult res;
    unsigned int n;
    std::vector<const char*> instance_extensions;
    if (SDL_Vulkan_GetInstanceExtensions(m_window, &n, nullptr) == SDL_FALSE) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        std::terminate();
    }
    instance_extensions.resize(n);
    if (SDL_Vulkan_GetInstanceExtensions(m_window, &n, instance_extensions.data()) == SDL_FALSE) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        std::terminate();
    }
    if (ENABLE_VALIDATION_LAYERS)
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags instance_create_flags = 0;
    std::vector<VkExtensionProperties> available_instance_extensions;
    if ((res = vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr)) != VK_SUCCESS) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "vkEnumerateInstanceExtensionProperties: %d", res);
        std::terminate();
    }
    available_instance_extensions.resize(n);
    if ((res = vkEnumerateInstanceExtensionProperties(nullptr, &n, available_instance_extensions.data())) != VK_SUCCESS) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "vkEnumerateInstanceExtensionProperties: %d", res);
        std::terminate();
    }
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
}

void VulkanRenderer::create_debug_messenger()
{
    if (ENABLE_VALIDATION_LAYERS == false)
        return;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    auto vkCreateDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (s_vkDestroyDebugUtilsMessenger == nullptr)
        s_vkDestroyDebugUtilsMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkCreateDebugUtilsMessenger == nullptr || s_vkDestroyDebugUtilsMessenger == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "vulkan extension " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " not present");
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
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s: skipping: no queue family supports graphics, compute, and presentation", device_props.deviceName);
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
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s: skipping: missing required extension %s", device_props.deviceName, missing_ext.c_str());
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

#define DEMAND_FEATURE(STRUCTURE, FIELD)                                                                                                \
    if (STRUCTURE.FIELD == VK_FALSE) {                                                                                                  \
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s: skipping: required feature " #FIELD " not available", device_props.deviceName); \
        return false;                                                                                                                   \
    }
    DEMAND_FEATURE(available_features.features, sampleRateShading);
    if (has_portability_subset) {
        DEMAND_FEATURE(portability_features, constantAlphaColorBlendFactors);
        DEMAND_FEATURE(portability_features, events);
    }
#undef DEMAND_FEATURE

    uint32_t surface_format_count, surface_present_mode_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(hwd, surface, &surface_format_count, nullptr);
    if (surface_format_count == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s: skipping: no supported surface formats", device_props.deviceName);
        return false;
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(hwd, surface, &surface_present_mode_count, nullptr);
    if (surface_present_mode_count == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "%s: skipping: no supported surface present modes", device_props.deviceName);
        return false;
    }

    return true;
}

void VulkanRenderer::pick_physical_device()
{
    uint32_t device_count = 0;
    std::vector<VkPhysicalDevice> devices;
    vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());

    for (auto& device : devices) {
        VkPhysicalDeviceProperties device_props;
        vkGetPhysicalDeviceProperties(device, &device_props);
        if (evaluate_physical_device(device, m_surface, device_props)) {
            m_hwd = device;
            if (device_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                return;
        }
    }
}

constexpr static float s_queue_priorities[] = { 1.0f, 0.5f };
void VulkanRenderer::create_logical_device()
{
    uint32_t count = 0;
    std::vector<VkExtensionProperties> available_exts;
    std::vector<const char*> extensions;
    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, nullptr);
    available_exts.resize(count);
    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, available_exts.data());

    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_device_local_memory {};
    pageable_device_local_memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT;

    for (auto& ext : available_exts) {
        if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (strcmp(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, ext.extensionName) == 0) {
            extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
            extensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
            pageable_device_local_memory.pageableDeviceLocalMemory = VK_TRUE;
        }
    }

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(m_hwd, &properties);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "selecting device %s", properties.deviceName);
    memcpy(&m_device_limits, &properties.limits, sizeof(VkPhysicalDeviceLimits));

    VkPhysicalDeviceVulkan12Features available_features12 {};
    available_features12.sType = m_device_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    m_device_features12.pNext = &pageable_device_local_memory;
    VkPhysicalDeviceVulkan11Features available_features11 {};
    available_features11.sType = m_device_features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    available_features11.pNext = &available_features12;
    m_device_features11.pNext = &m_device_features12;
    VkPhysicalDeviceFeatures2 available_features {};
    available_features.sType = m_device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    available_features.pNext = &available_features11;
    m_device_features.pNext = &m_device_features11;
    vkGetPhysicalDeviceFeatures2(m_hwd, &available_features);

    m_device_features.features.depthClamp = true;
    if (available_features.features.samplerAnisotropy)
        m_device_features.features.samplerAnisotropy = true;
    // Enable features in features{,11,12} if they're on in
    // available_features{,11,12}.

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
    createinfo.pNext = &m_device_features;
    createinfo.queueCreateInfoCount = ++queue_family_count;
    createinfo.pQueueCreateInfos = queue_createinfos.data();
    createinfo.enabledExtensionCount = extensions.size();
    createinfo.ppEnabledExtensionNames = extensions.data();
    createinfo.pEnabledFeatures = nullptr;
    VK_CHECK(vkCreateDevice(m_hwd, &createinfo, nullptr, &m_device));

    m_queue_families[static_cast<size_t>(QueueFamily::Universal)] = qf_primary;
    m_queue_families[static_cast<size_t>(QueueFamily::Compute)] = qf_compute;
    m_queue_families[static_cast<size_t>(QueueFamily::Transfer)] = qf_transfer;
    vkGetDeviceQueue(m_device, qf_primary, 0, &m_queues[static_cast<size_t>(QueueFamily::Universal)]);
    if (qf_compute != UINT32_MAX)
        vkGetDeviceQueue(m_device, qf_compute, 0, &m_queues[static_cast<size_t>(QueueFamily::Compute)]);
    if (qf_transfer != UINT32_MAX)
        vkGetDeviceQueue(m_device, qf_transfer, 0, &m_queues[static_cast<size_t>(QueueFamily::Transfer)]);
}

void VulkanRenderer::create_allocator()
{
    VmaAllocatorCreateInfo createinfo {};
    createinfo.flags = 0;
    createinfo.physicalDevice = m_hwd;
    createinfo.device = m_device;
    createinfo.instance = m_instance;
    createinfo.vulkanApiVersion = API_VERSION;
    VK_CHECK(vmaCreateAllocator(&createinfo, &m_allocator));
}

void VulkanRenderer::create_swapchain(VkSwapchainKHR old_swapchain)
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
}

void VulkanRenderer::create_render_pass()
{
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
    attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
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

void VulkanRenderer::create_framebuffers()
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
    i_createinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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

    i_createinfo.format = VK_FORMAT_D24_UNORM_S8_UINT;
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

void VulkanRenderer::create_pipeline_cache()
{
    VkPipelineCacheCreateInfo ci {};
    char* cidata = nullptr;
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    PHYSFS_Stat pcs;
    if (PHYSFS_stat(PIPELINE_CACHE_NAME, &pcs) != 0 && pcs.filetype == PHYSFS_FILETYPE_REGULAR && pcs.filesize > 0) {
        PHYSFS_File* fh = PHYSFS_openRead(PIPELINE_CACHE_NAME);
        cidata = new char[pcs.filesize];

        auto rc = PHYSFS_readBytes(fh, cidata, pcs.filesize);
        if (rc >= 0) {
            ci.initialDataSize = rc;
            ci.pInitialData = cidata;
        }
        PHYSFS_close(fh);
    }

    VK_CHECK(vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipeline_cache));
    delete[] cidata;
}

void VulkanRenderer::create_command_buffers()
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

    for (int i = 0; i < 2; i++) {
        VkCommandBufferAllocateInfo alloc_info {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = m_command_pools[i][static_cast<size_t>(QueueFamily::Universal)][0];
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 2;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &alloc_info, &m_command_buffers[i][0]));
    }
}

void VulkanRenderer::create_synchronizers()
{
    VkSemaphoreCreateInfo sem_ci {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_image_available[0]));
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_image_available[1]));
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_render_finished[0]));
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_render_finished[1]));
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_blit_finished[0]));
    VK_CHECK(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_blit_finished[1]));

    VkFenceCreateInfo fence_ci {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(m_device, &fence_ci, nullptr, &m_fence_frame[0]));
    VK_CHECK(vkCreateFence(m_device, &fence_ci, nullptr, &m_fence_frame[1]));
}

void VulkanRenderer::create_sampler()
{
    VkSamplerCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = m_mip_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (m_device_features.features.samplerAnisotropy && m_requested_anisotropy > 0) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(m_requested_anisotropy, m_device_limits.maxSamplerAnisotropy);
    }
    ci.minLod = 0;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    if (m_active_sampler != VK_NULL_HANDLE)
        vfree(m_active_sampler);
    VK_CHECK(vkCreateSampler(m_device, &ci, nullptr, &m_active_sampler));
}

void VulkanRenderer::release_freed_items(int bucket)
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

void VulkanRenderer::recreate_swapchain()
{
    vfree(m_swapchain);
    for (size_t i = 0; i < 2; i++) {
        vfree(m_framebuffers[i]);
        vfree(m_render_att_views[i]);
        vfree(m_render_atts[i]);
        vfree(m_render_att_allocs[i]);
    }

    VkSwapchainKHR old_swapchain = m_swapchain;
    vkDeviceWaitIdle(m_device);
    create_swapchain(old_swapchain);
    create_framebuffers();
}

void VulkanRenderer::write_pipeline_cache()
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

void VulkanRenderer::wait_idle()
{
    vkDeviceWaitIdle(m_device);
}

}