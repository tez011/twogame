cmake_minimum_required(VERSION 3.21)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(twogame
    "main.cpp"
    "twogame.cpp"
    "util.cpp"
    "asset/animation.cpp"
    "asset/image.cpp"
    "asset/manager.cpp"
    "asset/material.cpp"
    "asset/mesh.cpp"
    "asset/shader.cpp"
    "asset/xml.cpp"
    "scene/scene.cpp"
    "scene/xml.cpp"
    "vk/allocator.cpp"
    "vk/frame.cpp"
    "vk/pool.cpp"
    "vk/render_init.cpp"
    "vk/util.cpp"
)
target_link_libraries(twogame
    cglm
    EnTT::EnTT
    FastFloat::fast_float
    ktx_read
    PhysFS::PhysFS
    pugixml
    SDL2::SDL2
    SDL2::SDL2main
    spdlog
    spirv-reflect-static
    volk
    VulkanMemoryAllocator
    xxhash)
add_dependencies(twogame shaders)

if(MSVC)
target_compile_options(twogame PUBLIC "/EHsc" "/GR-")
else(MSVC)
target_compile_options(twogame PUBLIC -Wall)
target_compile_options(twogame PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif(MSVC)
