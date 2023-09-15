cmake_minimum_required(VERSION 3.20)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(twogame
    "main.cpp"
    "twogame.cpp"
    "fs.cpp"
    "vk/allocator.cpp"
    "vk/render_init.cpp")
target_link_libraries(twogame
    EnTT::EnTT
    ktx_read
    PhysFS::PhysFS
    pugixml
    SDL2::SDL2
    SDL2::SDL2main
    spirv-reflect-static
    Vulkan::Vulkan
    VulkanMemoryAllocator)

if(MSVC)
target_compile_options(twogame PUBLIC "/EHsc" "/GR-")
else(MSVC)
target_compile_options(twogame PUBLIC -Wall)
target_compile_options(twogame PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif(MSVC)