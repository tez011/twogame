#include "util.h"
#include <string_view>
#include <volk.h>

namespace twogame::vk {

#define P(T, S)        \
    if (name == (S)) { \
        out = T;       \
        return true;   \
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

size_t format_width(VkFormat fmt)
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

namespace twogame::util {

template <>
bool parse(std::string_view name, VkFormat& fmt)
{
#define X(FMT, SIZE, COMPONENTS) \
    if (name == #FMT) {          \
        fmt = VK_FORMAT_##FMT;   \
        return true;             \
    }
    VK_FORMATS
#undef X
    return false;
}

template <>
bool parse(std::string_view name, VkPrimitiveTopology& out)
{
    P(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, "triangles");
    P(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY, "triangles-adj");
    return false;
}

}
