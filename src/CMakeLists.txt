cmake_minimum_required(VERSION 3.31)

add_library(twogame STATIC
    "init.cpp"
    "vk/allocator.cpp"
    "vk/displayhost.cpp"
    "vk/renderer.cpp"
    "vk/scene.cpp")
target_include_directories(twogame PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(twogame
    embedded_shaders
    ktx_read
    physfs
    SDL3::SDL3
    Vulkan::Vulkan
    Vulkan::volk
    VulkanMemoryAllocator)

if(MSVC)
target_compile_options(twogame PUBLIC "/EHsc" "/GR-" "/W4")
else(MSVC)
target_compile_options(twogame PUBLIC "-Wall" "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
endif(MSVC)
