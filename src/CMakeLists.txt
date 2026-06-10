cmake_minimum_required(VERSION 3.31)

add_executable(twogame
    "allocator.cpp"
    "asset.cpp"
    "displayhost.cpp"
    "renderer.cpp"
    "scene.cpp"
    "main.cpp")
target_link_libraries(twogame
    cglm_headers
    embedded_shaders
    flatbuffers
    ktx_read
    physfs
    schema
    SDL3::SDL3
    Vulkan::Vulkan
    Vulkan::volk
    VulkanMemoryAllocator)

if(MSVC)
set_source_files_properties("allocator.cpp" PROPERTIES COMPILE_FLAGS "/W0")
target_compile_options(twogame PUBLIC "/EHsc" "/GR-" "/W4")
else(MSVC)
set_source_files_properties("allocator.cpp" PROPERTIES COMPILE_FLAGS "-w")
target_compile_options(twogame PUBLIC "-Wall" "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
endif(MSVC)
