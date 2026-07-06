#pragma once
#include <cstddef>
#include <cstdint>

namespace twogame {

struct Constants {
    constexpr static int FRAMES_IN_FLIGHT = 2;
    constexpr static uint32_t PICTUREBOOK_CAPACITY = 4096;
    constexpr static size_t STAGING_BUFFER_SIZE = 1 << 29;
    constexpr static float VERTICAL_FOV = 70.f;
};

}
