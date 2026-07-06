#pragma once
#include <SDL3/SDL_assert.h>

#ifdef DEBUG_BUILD
#include <SDL3/SDL_log.h>
#include <volk.h>
#include <vulkan/vk_enum_string_helper.h>
#define VK_DEMAND(X) VK_DEMAND_4P(X, SDL_FUNCTION, SDL_FILE, SDL_LINE)
#define VK_DEMAND_4P(X, N, F, L)                                                                                       \
    do {                                                                                                               \
        VkResult __r;                                                                                                  \
        if ((__r = (X)) != VK_SUCCESS) {                                                                               \
            SDL_LogCritical(SDL_LOG_CATEGORY_GPU, "assert in %s at %s:%d: %s: %s", N, F, L, #X, string_VkResult(__r)); \
            SDL_TriggerBreakpoint();                                                                                   \
        }                                                                                                              \
    } while (0)
#else
#define VK_DEMAND(X)                 \
    do {                             \
        if ((X) != VK_SUCCESS) {     \
            SDL_TriggerBreakpoint(); \
        }                            \
    } while (0)
#define SDL_LogTrace(...) ((void)0)
#define SDL_LogDebug(...) ((void)0)
#endif
