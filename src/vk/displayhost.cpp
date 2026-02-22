#define VK_ENABLE_BETA_EXTENSIONS
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>
#include <vulkan/vulkan_metal.h>
#include "display.h"
#include "scene.h"

#ifdef DEBUG_BUILD
constexpr static bool ENABLE_VALIDATION_LAYERS = true;
constexpr static const char* INSTANCE_LAYERS[] = { "VK_LAYER_KHRONOS_validation" };
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 1;
#else
constexpr static bool ENABLE_VALIDATION_LAYERS = false;
constexpr static const char** INSTANCE_LAYERS = nullptr;
constexpr static uint32_t INSTANCE_LAYERS_COUNT = 0;
#endif

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* cb_data, void* user_data)
{
    SDL_LogPriority priority;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        priority = SDL_LOG_PRIORITY_ERROR;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        priority = SDL_LOG_PRIORITY_WARN;
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        && ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0))
        priority = SDL_LOG_PRIORITY_INFO;
    else
        priority = SDL_LOG_PRIORITY_DEBUG;

    SDL_LogMessage(SDL_LOG_CATEGORY_GPU, priority, "%s", cb_data->pMessage);

#ifdef DEBUG_BUILD
    if (priority == SDL_LOG_PRIORITY_ERROR)
        std::abort();
#endif
    return VK_FALSE;
}

namespace twogame {

std::unique_ptr<DisplayHost> DisplayHost::s_self;

DisplayHost::DisplayHost()
{
    bool success = create_instance()
        && create_debug_messenger()
        && create_surface()
        && pick_physical_device()
        && create_logical_device()
        && create_pipeline_artifacts()
        && create_swapchain(VK_NULL_HANDLE)
        && create_syncobjects();

    if (!success)
        throw std::runtime_error("twogame::DisplayHost");
}

DisplayHost::~DisplayHost()
{
    vkDeviceWaitIdle(m_device);

    for (auto it = m_fence_frame.begin(); it != m_fence_frame.end(); ++it)
        vkDestroyFence(m_device, *it, nullptr);
    for (auto it = m_sem_submit_image.begin(); it != m_sem_submit_image.end(); ++it)
        vkDestroySemaphore(m_device, *it, nullptr);
    for (auto it = m_sem_acquire_image.begin(); it != m_sem_acquire_image.end(); ++it)
        vkDestroySemaphore(m_device, *it, nullptr);
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
    vkDestroyCommandPool(m_device, m_present_command_pool, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    SDL_DestroyWindow(m_window);
    if (m_debug_messenger != VK_NULL_HANDLE) {
        auto vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

void DisplayHost::init()
{
    SDL_assert(!s_self);
    s_self = std::unique_ptr<DisplayHost> { new DisplayHost };
}

void DisplayHost::drop()
{
    SDL_assert(s_self);
    s_self.reset();
}

bool DisplayHost::create_instance()
{
    Uint32 count;
    const char* const* base_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (base_extensions == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return false;
    }
    std::vector<const char*> instance_extensions(base_extensions, base_extensions + count);
    if (ENABLE_VALIDATION_LAYERS)
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags instance_create_flags = 0;
    if (std::any_of(instance_extensions.begin(), instance_extensions.end(),
            [](const char* name) { return strcmp(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, name) == 0; }))
        instance_create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

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
    VK_DEMAND(vkCreateInstance(&createinfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    return true;
}

bool DisplayHost::create_debug_messenger()
{
#if DEBUG_BUILD
    auto vkCreateDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (vkCreateDebugUtilsMessenger == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, VK_EXT_DEBUG_UTILS_EXTENSION_NAME " not present; skipping debug messenger creation");
        return false;
    }

    VkDebugUtilsMessengerCreateInfoEXT createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createinfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createinfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createinfo.pfnUserCallback = vk_debug_callback;

    VK_DEMAND(vkCreateDebugUtilsMessenger(m_instance, &createinfo, nullptr, &m_debug_messenger));
#endif
    return true;
}

bool DisplayHost::create_surface()
{
    const char* title = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING);
    if (title == nullptr)
        title = "twogame";
    if ((m_window = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE)) == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    Uint32 count;
    const char* const* base_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (std::any_of(base_extensions, base_extensions + count,
            [](const char* name) { return strcmp(VK_EXT_METAL_SURFACE_EXTENSION_NAME, name) == 0; })) {
        // If metal backend, create the surface directly
        SDL_MetalView metal_view = SDL_Metal_CreateView(m_window);
        auto vkCreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(m_instance, "vkCreateMetalSurfaceEXT");

        VkMetalSurfaceCreateInfoEXT createinfo {};
        createinfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        createinfo.pLayer = SDL_Metal_GetLayer(metal_view);
        VK_DEMAND(vkCreateMetalSurfaceEXT(m_instance, &createinfo, nullptr, &m_surface));
    } else {
        if (SDL_Vulkan_CreateSurface(m_window, m_instance, nullptr, &m_surface) == false) {
            SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

bool DisplayHost::pick_physical_device()
{
    uint32_t device_count;
    VK_DEMAND(vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr));
    if (device_count == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "no Vulkan-capable devices were available");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    std::vector<float> scores(device_count);
    VK_DEMAND(vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()));
    std::transform(devices.begin(), devices.end(), scores.begin(), [this](VkPhysicalDevice hwd) {
        VkPhysicalDeviceProperties2 hwd_props {};
        hwd_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vkGetPhysicalDeviceProperties2(hwd, &hwd_props);

        uint32_t count;
        std::vector<VkQueueFamilyProperties2> queue_family_props;
        vkGetPhysicalDeviceQueueFamilyProperties2(hwd, &count, nullptr);
        queue_family_props.resize(count);
        for (uint32_t i = 0; i < count; i++)
            queue_family_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        vkGetPhysicalDeviceQueueFamilyProperties2(hwd, &count, queue_family_props.data());

        int qfi = -1;
        for (uint32_t i = 0; i < count; i++) {
            const auto& props = queue_family_props[i].queueFamilyProperties;
            if (qfi == -1) {
                if ((props.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    VkBool32 can_present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(hwd, i, m_surface, &can_present);
                    if (can_present && props.queueCount > 0)
                        qfi = i;
                }
            }
        }
        if (qfi == -1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: no queue capable of graphics, compute, and presentation", hwd_props.properties.deviceName);
            return 0.f;
        }

        vkEnumerateDeviceExtensionProperties(hwd, nullptr, &count, nullptr);

        bool has_portability_subset = false;
        std::vector<VkExtensionProperties> available_extensions(count);
        std::set<std::string_view> missing_extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        vkEnumerateDeviceExtensionProperties(hwd, nullptr, &count, available_extensions.data());
        for (const auto& ext : available_extensions) {
            if (strcmp(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, ext.extensionName) == 0)
                has_portability_subset = true;
            auto it = missing_extensions.find(ext.extensionName);
            if (it != missing_extensions.end())
                missing_extensions.erase(it);
        }
        for (std::string_view extension : missing_extensions)
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: required extension %s missing", hwd_props.properties.deviceName, extension.data());
        if (missing_extensions.size() > 0)
            return 0.f;

        VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_features {};
        portability_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
        VkPhysicalDeviceVulkan13Features available_features13 {};
        available_features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        available_features13.pNext = has_portability_subset ? &portability_features : nullptr;
        VkPhysicalDeviceVulkan12Features available_features12 {};
        available_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        available_features12.pNext = &available_features13;
        VkPhysicalDeviceVulkan11Features available_features11 {};
        available_features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        available_features11.pNext = &available_features12;
        VkPhysicalDeviceFeatures2 available_features {};
        available_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        available_features.pNext = &available_features11;
        vkGetPhysicalDeviceFeatures2(hwd, &available_features);
#define DEMAND_FEATURE(STRUCTURE, FIELD)                                                                                              \
    if (STRUCTURE.FIELD == VK_FALSE) {                                                                                                \
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: required feature %s not available", hwd_props.properties.deviceName, #FIELD); \
        return 0.f;                                                                                                                   \
    }

        DEMAND_FEATURE(available_features.features, depthClamp);
        DEMAND_FEATURE(available_features12, descriptorBindingSampledImageUpdateAfterBind);
        DEMAND_FEATURE(available_features12, descriptorBindingVariableDescriptorCount);
        DEMAND_FEATURE(available_features12, descriptorIndexing);
        DEMAND_FEATURE(available_features12, timelineSemaphore);
        DEMAND_FEATURE(available_features12, uniformBufferStandardLayout);
        DEMAND_FEATURE(available_features13, synchronization2);
        if (has_portability_subset) {
            DEMAND_FEATURE(portability_features, constantAlphaColorBlendFactors);
        }
#undef DEMAND_FEATURE

        VkImageFormatProperties ifmt;
        uint32_t surface_format_count, surface_present_mode_count;
        vkGetPhysicalDeviceSurfaceFormatsKHR(hwd, m_surface, &surface_format_count, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(hwd, m_surface, &surface_present_mode_count, nullptr);
        if (surface_format_count == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: no supported surface formats", hwd_props.properties.deviceName);
            return 0.f;
        }
        if (surface_present_mode_count == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: no supported surface present modes", hwd_props.properties.deviceName);
            return 0.f;
        }
        if (vkGetPhysicalDeviceImageFormatProperties(hwd, VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ifmt) == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: required image format BGRA8_SRGB is not supported", hwd_props.properties.deviceName);
            return 0.f;
        }
        if (vkGetPhysicalDeviceImageFormatProperties(hwd, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, &ifmt) == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: required image format RGBA32F is not supported", hwd_props.properties.deviceName);
            return 0.f;
        }
        if (vkGetPhysicalDeviceImageFormatProperties(hwd, DEPTH_FORMAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ifmt) == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "skipping %s: required depth format D32F is not supported", hwd_props.properties.deviceName);
            return 0.f;
        }

        VkDeviceSize memtotal = 0;
        VkPhysicalDeviceMemoryProperties mem_props {};
        vkGetPhysicalDeviceMemoryProperties(hwd, &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
            if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                memtotal += mem_props.memoryHeaps[i].size;
            }
        }

        float score = std::log2(memtotal);
        if (hwd_props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 2.f;
        return score;
    });

    auto argmax = std::distance(std::max_element(scores.begin(), scores.begin() + device_count), scores.begin());
    if (scores[argmax] > 0) {
        m_hwd = devices[argmax];
        return true;
    } else {
        SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "no Vulkan-capable devices met requirements");
        return false;
    }
}

bool DisplayHost::create_logical_device()
{
    std::vector<const char*> extensions;
    std::vector<VkExtensionProperties> available_extensions;
    uint32_t count;

    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, nullptr);
    available_extensions.resize(count);
    vkEnumerateDeviceExtensionProperties(m_hwd, nullptr, &count, available_extensions.data());
    for (auto& ext : available_extensions) {
        if (strcmp(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (strcmp(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, ext.extensionName) == 0)
            extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }

    VkPhysicalDeviceDriverProperties driver {};
    VkPhysicalDeviceProperties2 properties {};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(m_hwd, &properties);
    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "selecting device %s via %s", properties.properties.deviceName, driver.driverName);
#ifdef DEBUG_BUILD
    for (auto& e : extensions)
        SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "    with %s", e);
#endif

    // Enable all available features.
    VkPhysicalDeviceFeatures2 device_features {};
    VkPhysicalDeviceVulkan11Features device_features11 {};
    VkPhysicalDeviceVulkan12Features device_features12 {};
    VkPhysicalDeviceVulkan13Features device_features13 {};
    VkPhysicalDeviceRobustness2FeaturesEXT device_features_robustness2 {};
    device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device_features.pNext = &device_features11;
    device_features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    device_features11.pNext = &device_features12;
    device_features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    device_features12.pNext = &device_features13;
    device_features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    device_features13.pNext = &device_features_robustness2;
    device_features_robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    vkGetPhysicalDeviceFeatures2(m_hwd, &device_features);
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, nullptr);
#ifdef __APPLE__
    device_features.features.robustBufferAccess = VK_FALSE;
#endif

    float queue_priority = 1.0f;
    int queue_createinfo_count = 0;
    std::array<VkDeviceQueueCreateInfo, 2> queue_createinfos {};
    std::vector<VkQueueFamilyProperties> queue_families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_hwd, &count, queue_families.data());

    uint32_t qfi = 0, qfi_dma = 0;
    VkDeviceSize image_transfer_granularity = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (queue_families[i].queueCount == 0)
            continue;
        if (qfi == 0 && (queue_families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_hwd, i, m_surface, &can_present);
            if (can_present)
                qfi = i + 1;
        }
        if ((queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && qfi != i + 1) {
            VkDeviceSize gr = queue_families[i].minImageTransferGranularity.width * queue_families[i].minImageTransferGranularity.height * queue_families[i].minImageTransferGranularity.depth;
            if (image_transfer_granularity > gr) {
                image_transfer_granularity = gr;
                qfi_dma = i + 1;
            }
        }
    }
    m_queue_family_index = qfi - 1;
    m_dma_queue_family_index = (qfi_dma ? qfi_dma : qfi) - 1;
    queue_createinfos[queue_createinfo_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_createinfos[queue_createinfo_count].queueFamilyIndex = m_queue_family_index;
    queue_createinfos[queue_createinfo_count].queueCount = 1;
    queue_createinfos[queue_createinfo_count].pQueuePriorities = &queue_priority;
    if (qfi_dma && qfi_dma != qfi) {
        queue_createinfos[++queue_createinfo_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_createinfos[queue_createinfo_count].queueFamilyIndex = m_dma_queue_family_index;
        queue_createinfos[queue_createinfo_count].queueCount = 1;
        queue_createinfos[queue_createinfo_count].pQueuePriorities = &queue_priority;
    }

    VkDeviceCreateInfo createinfo {};
    createinfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createinfo.pNext = &device_features;
    createinfo.queueCreateInfoCount = ++queue_createinfo_count;
    createinfo.pQueueCreateInfos = queue_createinfos.data();
    createinfo.enabledExtensionCount = extensions.size();
    createinfo.ppEnabledExtensionNames = extensions.data();
    VK_DEMAND(vkCreateDevice(m_hwd, &createinfo, nullptr, &m_device));
    volkLoadDevice(m_device);

    VmaAllocatorCreateInfo allocator_ci {};
    VmaVulkanFunctions vfn {};
    allocator_ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocator_ci.physicalDevice = m_hwd;
    allocator_ci.device = m_device;
    allocator_ci.instance = m_instance;
    allocator_ci.vulkanApiVersion = API_VERSION;
    allocator_ci.pVulkanFunctions = &vfn;
    vfn.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vfn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VK_DEMAND(vmaCreateAllocator(&allocator_ci, &m_allocator));

    return true;
}

bool DisplayHost::create_pipeline_artifacts()
{
    VkCommandPoolCreateInfo pool_ci {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = m_queue_family_index;
    VK_DEMAND(vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_present_command_pool));

    VkCommandBufferAllocateInfo command_allocinfo {};
    command_allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocinfo.commandPool = m_present_command_pool;
    command_allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocinfo.commandBufferCount = m_present_commands.size();
    VK_DEMAND(vkAllocateCommandBuffers(m_device, &command_allocinfo, m_present_commands.data()));

    VkPipelineCacheCreateInfo pipeline_cache_createinfo {};
    pipeline_cache_createinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    pipeline_cache_createinfo.flags = VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
    VK_DEMAND(vkCreatePipelineCache(m_device, &pipeline_cache_createinfo, nullptr, &m_pipeline_cache));

    return true;
}

bool DisplayHost::create_swapchain(VkSwapchainKHR old_swapchain)
{
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_hwd, m_surface, &capabilities);

    uint32_t count;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_hwd, m_surface, &count, nullptr);
    formats.resize(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_hwd, m_surface, &count, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_hwd, m_surface, &count, nullptr);
    present_modes.resize(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_hwd, m_surface, &count, present_modes.data());

    auto fmt_it = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& fmt) {
        return fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (fmt_it == formats.end())
        fmt_it = formats.begin();

    if (capabilities.currentExtent.width == std::numeric_limits<uint32_t>::max()) {
        int w, h;
        if (SDL_GetWindowSizeInPixels(m_window, &w, &h) == false) {
            SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "SDL_GetWindowSizeInPixels: %s", SDL_GetError());
            return false;
        }

        m_swapchain_extent.width = std::clamp(static_cast<uint32_t>(w), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        m_swapchain_extent.height = std::clamp(static_cast<uint32_t>(w), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
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
    createinfo.imageFormat = fmt_it->format;
    createinfo.imageColorSpace = fmt_it->colorSpace;
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
    VK_DEMAND(vkCreateSwapchainKHR(m_device, &createinfo, nullptr, &m_swapchain));

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data());
    m_swapchain_format = fmt_it->format;
    return true;
}

bool DisplayHost::create_syncobjects()
{
    VkSemaphoreCreateInfo sem_ci {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_sem_acquire_image[i]));
    }

    m_sem_submit_image.resize(m_swapchain_images.size());
    for (auto it = m_sem_submit_image.begin(); it != m_sem_submit_image.end(); ++it)
        VK_DEMAND(vkCreateSemaphore(m_device, &sem_ci, nullptr, &*it));

    VkFenceCreateInfo fence_ci {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < SIMULTANEOUS_FRAMES; i++) {
        VK_DEMAND(vkCreateFence(m_device, &fence_ci, nullptr, &m_fence_frame[i]));
    }

    return true;
}

bool DisplayHost::recreate_swapchain()
{
    VkSwapchainKHR old_swapchain = m_swapchain;
    vkDeviceWaitIdle(m_device);

    bool success = create_swapchain(old_swapchain);
    if (success)
        vkDestroySwapchainKHR(m_device, old_swapchain, nullptr);
    m_swapchain_recreated = true;
    return success;
}

int32_t DisplayHost::acquire_image()
{
    uint32_t next_frame_number = m_frame_number.load(std::memory_order_relaxed) + 1;
    VkFence fence = m_fence_frame[next_frame_number % SIMULTANEOUS_FRAMES];
    VK_DEMAND(vkWaitForFences(m_device, 1, &fence, VK_FALSE, UINT64_MAX));
    VK_DEMAND(vkResetFences(m_device, 1, &fence));
    SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u END", next_frame_number - 1);
    m_frame_number.store(next_frame_number, std::memory_order_release);
    m_frame_number.notify_all();

    VkResult res;
    uint32_t index;
    VkSemaphore sem = m_sem_acquire_image[next_frame_number % SIMULTANEOUS_FRAMES];
    do {
        if ((res = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index)) == VK_ERROR_OUT_OF_DATE_KHR) {
            if (recreate_swapchain() == false) {
                return -1;
            }
        }
    } while (res == VK_ERROR_OUT_OF_DATE_KHR);

    if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
        SDL_LogTrace(SDL_LOG_CATEGORY_SYSTEM, "render thread: H%u BEGIN -> I%u\n", next_frame_number, index);
        return index;
    } else {
        SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "failed to acquire swapchain image: %s", string_VkResult(res));
        return -1;
    }
}

void DisplayHost::present_image(uint32_t index, VkImage image, VkSemaphore signal)
{
    uint32_t frame_number = m_frame_number.load(std::memory_order_relaxed);
    VkQueue queue;
    vkGetDeviceQueue(m_device, m_queue_family_index, 0, &queue);

    VkCommandBuffer present_commands = m_present_commands[frame_number % SIMULTANEOUS_FRAMES];
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_DEMAND(vkBeginCommandBuffer(present_commands, &begin_info));

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_swapchain_images[index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(present_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageBlit blit {};
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[0] = blit.srcOffsets[1] = { 0, 0, 0 };
    blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(m_swapchain_extent.width), static_cast<int32_t>(m_swapchain_extent.height), 1 };
    vkCmdBlitImage(present_commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(present_commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VK_DEMAND(vkEndCommandBuffer(present_commands));

    auto wait_sems = std::to_array<VkSemaphore>({ m_sem_acquire_image[frame_number % SIMULTANEOUS_FRAMES], signal });
    auto wait_stages = std::to_array<VkPipelineStageFlags>({ VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT });
    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = wait_sems.size();
    submit.pWaitSemaphores = wait_sems.data();
    submit.pWaitDstStageMask = wait_stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &present_commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_sem_submit_image[index];
    VK_DEMAND(vkQueueSubmit(queue, 1, &submit, m_fence_frame[frame_number % SIMULTANEOUS_FRAMES]));

    VkPresentInfoKHR present {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_sem_submit_image[index];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &index;

    VkResult res = vkQueuePresentKHR(queue, &present);
    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
    } else if (res != VK_SUCCESS) {
        std::abort();
    }
}

SDL_AppResult DisplayHost::draw_frame()
{
    IRenderer* renderer = SceneHost::renderer();
    int32_t swapchain_slot = acquire_image();
    if (swapchain_slot < 0)
        return SDL_APP_FAILURE;
    if (m_swapchain_recreated) {
        renderer->resize_frames(m_swapchain_extent);
        renderer->recreate_subpass_data(m_frame_number);
        m_swapchain_recreated = false;
    }

    IRenderer::Output output = renderer->draw(m_frame_number);
    present_image(swapchain_slot, output.image, output.signal);
    SceneHost::submit_transfers();

    return SDL_APP_CONTINUE;
}

#define VK_FORMATS                      \
    X(R4G4_UNORM_PACK8, 1, 2)           \
    X(R4G4B4A4_UNORM_PACK16, 2, 4)      \
    X(B4G4R4A4_UNORM_PACK16, 2, 4)      \
    X(R5G6B5_UNORM_PACK16, 2, 3)        \
    X(B5G6R5_UNORM_PACK16, 2, 3)        \
    X(R5G5B5A1_UNORM_PACK16, 2, 4)      \
    X(B5G5R5A1_UNORM_PACK16, 2, 4)      \
    X(A1R5G5B5_UNORM_PACK16, 2, 4)      \
    X(R8_UNORM, 1, 1)                   \
    X(R8_SNORM, 1, 1)                   \
    X(R8_USCALED, 1, 1)                 \
    X(R8_SSCALED, 1, 1)                 \
    X(R8_UINT, 1, 1)                    \
    X(R8_SINT, 1, 1)                    \
    X(R8_SRGB, 1, 1)                    \
    X(R8G8_UNORM, 2, 2)                 \
    X(R8G8_SNORM, 2, 2)                 \
    X(R8G8_USCALED, 2, 2)               \
    X(R8G8_SSCALED, 2, 2)               \
    X(R8G8_UINT, 2, 2)                  \
    X(R8G8_SINT, 2, 2)                  \
    X(R8G8_SRGB, 2, 2)                  \
    X(R8G8B8_UNORM, 3, 3)               \
    X(R8G8B8_SNORM, 3, 3)               \
    X(R8G8B8_USCALED, 3, 3)             \
    X(R8G8B8_SSCALED, 3, 3)             \
    X(R8G8B8_UINT, 3, 3)                \
    X(R8G8B8_SINT, 3, 3)                \
    X(R8G8B8_SRGB, 3, 3)                \
    X(B8G8R8_UNORM, 3, 3)               \
    X(B8G8R8_SNORM, 3, 3)               \
    X(B8G8R8_USCALED, 3, 3)             \
    X(B8G8R8_SSCALED, 3, 3)             \
    X(B8G8R8_UINT, 3, 3)                \
    X(B8G8R8_SINT, 3, 3)                \
    X(B8G8R8_SRGB, 3, 3)                \
    X(R8G8B8A8_UNORM, 4, 4)             \
    X(R8G8B8A8_SNORM, 4, 4)             \
    X(R8G8B8A8_USCALED, 4, 4)           \
    X(R8G8B8A8_SSCALED, 4, 4)           \
    X(R8G8B8A8_UINT, 4, 4)              \
    X(R8G8B8A8_SINT, 4, 4)              \
    X(R8G8B8A8_SRGB, 4, 4)              \
    X(B8G8R8A8_UNORM, 4, 4)             \
    X(B8G8R8A8_SNORM, 4, 4)             \
    X(B8G8R8A8_USCALED, 4, 4)           \
    X(B8G8R8A8_SSCALED, 4, 4)           \
    X(B8G8R8A8_UINT, 4, 4)              \
    X(B8G8R8A8_SINT, 4, 4)              \
    X(B8G8R8A8_SRGB, 4, 4)              \
    X(A8B8G8R8_UNORM_PACK32, 4, 4)      \
    X(A8B8G8R8_SNORM_PACK32, 4, 4)      \
    X(A8B8G8R8_USCALED_PACK32, 4, 4)    \
    X(A8B8G8R8_SSCALED_PACK32, 4, 4)    \
    X(A8B8G8R8_UINT_PACK32, 4, 4)       \
    X(A8B8G8R8_SINT_PACK32, 4, 4)       \
    X(A8B8G8R8_SRGB_PACK32, 4, 4)       \
    X(A2R10G10B10_UNORM_PACK32, 4, 4)   \
    X(A2R10G10B10_SNORM_PACK32, 4, 4)   \
    X(A2R10G10B10_USCALED_PACK32, 4, 4) \
    X(A2R10G10B10_SSCALED_PACK32, 4, 4) \
    X(A2R10G10B10_UINT_PACK32, 4, 4)    \
    X(A2R10G10B10_SINT_PACK32, 4, 4)    \
    X(A2B10G10R10_UNORM_PACK32, 4, 4)   \
    X(A2B10G10R10_SNORM_PACK32, 4, 4)   \
    X(A2B10G10R10_USCALED_PACK32, 4, 4) \
    X(A2B10G10R10_SSCALED_PACK32, 4, 4) \
    X(A2B10G10R10_UINT_PACK32, 4, 4)    \
    X(A2B10G10R10_SINT_PACK32, 4, 4)    \
    X(R16_UNORM, 2, 1)                  \
    X(R16_SNORM, 2, 1)                  \
    X(R16_USCALED, 2, 1)                \
    X(R16_SSCALED, 2, 1)                \
    X(R16_UINT, 2, 1)                   \
    X(R16_SINT, 2, 1)                   \
    X(R16_SFLOAT, 2, 1)                 \
    X(R16G16_UNORM, 4, 2)               \
    X(R16G16_SNORM, 4, 2)               \
    X(R16G16_USCALED, 4, 2)             \
    X(R16G16_SSCALED, 4, 2)             \
    X(R16G16_UINT, 4, 2)                \
    X(R16G16_SINT, 4, 2)                \
    X(R16G16_SFLOAT, 4, 2)              \
    X(R16G16B16_UNORM, 6, 3)            \
    X(R16G16B16_SNORM, 6, 3)            \
    X(R16G16B16_USCALED, 6, 3)          \
    X(R16G16B16_SSCALED, 6, 3)          \
    X(R16G16B16_UINT, 6, 3)             \
    X(R16G16B16_SINT, 6, 3)             \
    X(R16G16B16_SFLOAT, 6, 3)           \
    X(R16G16B16A16_UNORM, 8, 4)         \
    X(R16G16B16A16_SNORM, 8, 4)         \
    X(R16G16B16A16_USCALED, 8, 4)       \
    X(R16G16B16A16_SSCALED, 8, 4)       \
    X(R16G16B16A16_UINT, 8, 4)          \
    X(R16G16B16A16_SINT, 8, 4)          \
    X(R16G16B16A16_SFLOAT, 8, 4)        \
    X(R32_UINT, 4, 1)                   \
    X(R32_SINT, 4, 1)                   \
    X(R32_SFLOAT, 4, 1)                 \
    X(R32G32_UINT, 8, 2)                \
    X(R32G32_SINT, 8, 2)                \
    X(R32G32_SFLOAT, 8, 2)              \
    X(R32G32B32_UINT, 12, 3)            \
    X(R32G32B32_SINT, 12, 3)            \
    X(R32G32B32_SFLOAT, 12, 3)          \
    X(R32G32B32A32_UINT, 16, 4)         \
    X(R32G32B32A32_SINT, 16, 4)         \
    X(R32G32B32A32_SFLOAT, 16, 4)       \
    X(R64_UINT, 8, 1)                   \
    X(R64_SINT, 8, 1)                   \
    X(R64_SFLOAT, 8, 1)                 \
    X(R64G64_UINT, 16, 2)               \
    X(R64G64_SINT, 16, 2)               \
    X(R64G64_SFLOAT, 16, 2)             \
    X(R64G64B64_UINT, 24, 3)            \
    X(R64G64B64_SINT, 24, 3)            \
    X(R64G64B64_SFLOAT, 24, 3)          \
    X(R64G64B64A64_UINT, 32, 4)         \
    X(R64G64B64A64_SINT, 32, 4)         \
    X(R64G64B64A64_SFLOAT, 32, 4)       \
    X(B10G11R11_UFLOAT_PACK32, 4, 3)    \
    X(E5B9G9R9_UFLOAT_PACK32, 4, 3)     \
    X(D16_UNORM, 2, 1)                  \
    X(X8_D24_UNORM_PACK32, 4, 1)        \
    X(D32_SFLOAT, 4, 1)                 \
    X(S8_UINT, 1, 1)                    \
    X(D16_UNORM_S8_UINT, 3, 2)          \
    X(D24_UNORM_S8_UINT, 4, 2)          \
    X(D32_SFLOAT_S8_UINT, 8, 2)

size_t DisplayHost::format_width(VkFormat fmt)
{
#define X(FMT, SIZE, COMPONENTS) \
    case VK_FORMAT_##FMT:        \
        return SIZE;
    switch (fmt) {
        VK_FORMATS
    default:
        return 0;
    }
#undef X
}

}
