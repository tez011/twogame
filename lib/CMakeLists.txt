cmake_minimum_required(VERSION 3.31)

# ---- external dependencies ----
CPMAddPackage(NAME glm
              VERSION 1.0.3
              GIT_REPOSITORY https://github.com/g-truc/glm.git
              GIT_TAG 1.0.3
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "GLM_ENABLE_CXX_17 ON"
                      "GLM_ENABLE_CXX_20 ON"
                      "GLM_ENABLE_FAST_MATH ON")
CPMAddPackage(NAME ktx
              VERSION 4.4.2
              GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "KTX_FEATURE_TESTS OFF"
                      "KTX_FEATURE_GL_UPLOAD OFF"
                      "KTX_FEATURE_VK_UPLOAD OFF")
CPMAddPackage(NAME physicsfs
              VERSION 3.2.0
              GIT_REPOSITORY https://github.com/icculus/physfs.git
              GIT_TAG release-3.2.0
              DOWNLOAD_ONLY TRUE)
CPMAddPackage(NAME pugixml
              VERSION 1.15
              GIT_REPOSITORY https://github.com/zeux/pugixml.git
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "BUILD_SHARED_LIBS OFF")
CPMAddPackage(NAME spirvreflect
              VERSION 1.4.335.0
              GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
              GIT_TAG vulkan-sdk-1.4.335.0
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "SPIRV_REFLECT_EXECUTABLE OFF"
                      "SPIRV_REFLECT_EXAMPLES OFF"
                      "SPIRV_REFLECT_STATIC_LIB ON")
CPMAddPackage(NAME vulkanmemoryallocator
              VERSION 3.3.0
              GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "VMA_BUILD_DOCUMENTATION OFF"
                      "VMA_BUILD_SAMPLES OFF"
                      "VMA_DYNAMIC_VULKAN_FUNCTIONS ON"
                      "VMA_STATIC_VULKAN_FUNCTIONS OFF")

# ---- library options ----
if(glm_ADDED)
    target_compile_definitions(glm PUBLIC "GLM_FORCE_INTRINSICS")
    target_compile_definitions(glm-header-only INTERFACE "GLM_FORCE_INTRINSICS")
endif()
if (physicsfs_ADDED)
    set(PHYSFS_EXTRA_SRCS)
    if(APPLE)
        list(APPEND PHYSFS_EXTRA_SRCS "${physicsfs_SOURCE_DIR}/src/physfs_platform_apple.m")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "WindowsPhone" OR CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
        set(WINRT TRUE)
        list(APPEND PHYSFS_EXTRA_SRCS "${physicsfs_SOURCE_DIR}/src/physfs_platform_winrt.cpp")
    endif()
    add_library(physfs STATIC
        ${physicsfs_SOURCE_DIR}/src/physfs.c
        ${physicsfs_SOURCE_DIR}/src/physfs_byteorder.c
        ${physicsfs_SOURCE_DIR}/src/physfs_unicode.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_posix.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_unix.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_windows.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_os2.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_qnx.c
        ${physicsfs_SOURCE_DIR}/src/physfs_platform_android.c
        ${physicsfs_SOURCE_DIR}/src/physfs_archiver_dir.c
        ${physicsfs_SOURCE_DIR}/src/physfs_archiver_unpacked.c
        ${physicsfs_SOURCE_DIR}/src/physfs_archiver_7z.c
        ${physicsfs_SOURCE_DIR}/src/physfs_archiver_zip.c
        ${PHYSFS_EXTRA_SRCS})
    if(WINRT)
        # Ignore LNK4264 warnings; we don't author any WinRT components, just consume them, so we're okay in a static library.
		set_target_properties(physfs PROPERTIES VS_WINRT_COMPONENT True)
        set_target_properties(physfs PROPERTIES STATIC_LIBRARY_FLAGS "/ignore:4264")
    endif()
    if(WIN32 OR WINRT OR OS2)
        # no dll exports from the static library
        target_compile_definitions(physfs PRIVATE "PHYSFS_STATIC")
    endif()
    if(APPLE)
        target_link_libraries(physfs PRIVATE "-framework IOKit" "-framework Foundation")
    endif()
    target_include_directories(physfs PUBLIC "$<BUILD_INTERFACE:${physicsfs_SOURCE_DIR}/src>")
    target_compile_definitions(physfs PRIVATE "PHYSFS_SUPPORTS_GRP=0"
                                              "PHYSFS_SUPPORTS_QPAK=0"
                                              "PHYSFS_SUPPORTS_HOG=0"
                                              "PHYSFS_SUPPORTS_MVL=0"
                                              "PHYSFS_SUPPORTS_WAD=0"
                                              "PHYSFS_SUPPORTS_SLB=0"
                                              "PHYSFS_SUPPORTS_ISO9660=0"
                                              "PHYSFS_SUPPORTS_VDF=0")
    set_property(TARGET physfs PROPERTY POSITION_INDEPENDENT_CODE TRUE)
endif()
