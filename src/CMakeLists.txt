cmake_minimum_required(VERSION 3.31)

add_executable(twogame
    "assets/animation.cpp"
    "assets/image.cpp"
    "assets/material.cpp"
    "assets/mesh.cpp"
    "assets/skeleton.cpp"
    "core/math.cpp"
    "render/allocator.cpp"
    "render/display_host.cpp"
    "render/renderer.cpp"
    "render/staging_buffer.cpp"
    "scene/scene_graph.cpp"
    "scene/scene_host.cpp"
    "scene/scene_manifest.cpp"
    "scene/scene.cpp"
    "main.cpp")
target_include_directories(twogame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
