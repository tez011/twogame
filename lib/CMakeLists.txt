cmake_minimum_required(VERSION 3.21)

# ---- library options ----
add_compile_definitions(GLM_FORCE_EXPLICIT_CTOR
                        $<$<CONFIG:RELEASE>:GLM_FORCE_INLINE>
                        GLM_FORCE_ALIGNED_GENTYPES
                        GLM_FORCE_INTRINSICS
                        GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(WIN32)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_MACOS_MVK)
else()
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
endif()

CPMAddPackage(NAME entt
              VERSION 3.12.2
              GIT_REPOSITORY https://github.com/skypjack/entt.git
              EXCLUDE_FROM_ALL TRUE)
CPMAddPAckage(NAME glm
              VERSION 0.9.9
              GIT_REPOSITORY https://github.com/g-truc/glm.git
              GIT_TAG 47585fde0c49fa77a2bf2fb1d2ead06999fd4b6e)
CPMAddPackage(NAME ktx
              VERSION 4.3.0-2cf053c193593f11c932ad742798ee1b101e50ee
              GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
              GIT_TAG 2cf053c193593f11c932ad742798ee1b101e50ee  # latest at time of writing, since 4.2.1 has build issue
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "KTX_FEATURE_STATIC_LIBRARY ON"
                      "KTX_FEATURE_TESTS OFF"
                      "KTX_FEATURE_VULKAN OFF"
                      "KTX_FEATURE_GL_UPLOAD OFF")
CPMAddPackage(NAME physicsfs
              VERSION 3.2.0
              GIT_REPOSITORY https://github.com/icculus/physfs.git
              GIT_TAG release-3.2.0
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "PHYSFS_ARCHIVE_ZIP TRUE"
                      "PHYSFS_ARCHIVE_7Z TRUE"
                      "PHYSFS_ARCHIVE_GRP FALSE"
                      "PHYSFS_ARCHIVE_WAD FALSE"
                      "PHYSFS_ARCHIVE_HOG FALSE"
                      "PHYSFS_ARCHIVE_MVL FALSE"
                      "PHYSFS_ARCHIVE_QPAK FALSE"
                      "PHYSFS_ARCHIVE_SLB FALSE"
                      "PHYSFS_ARCHIVE_ISO9660 FALSE"
                      "PHYSFS_ARCHIVE_VDF FALSE"
                      "PHYSFS_BUILD_TEST FALSE"
                      "PHYSFS_BUILD_DOCS FALSE"
                      "PHYSFS_DISABLE_INSTALL ON"
                      "PHYSFS_TARGETNAME_UNINSTALL PHYSFS_UNINSTALL_TGT_INT")
CPMAddPackage(NAME pugixml
              VERSION 1.13
              GIT_REPOSITORY https://github.com/zeux/pugixml.git
              EXCLUDE_FROM_ALL TRUE)
CPMAddPackage(NAME spdlog
              VERSION 1.12.0
              GIT_REPOSITORY https://github.com/gabime/spdlog.git)
CPMAddPackage(NAME spirvreflect
              VERSION 1.3.239.0
              GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
              GIT_TAG sdk-1.3.239.0
              OPTIONS "SPIRV_REFLECT_EXECUTABLE OFF"
                      "SPIRV_REFLECT_EXAMPLES OFF"
                      "SPIRV_REFLECT_STATIC_LIB ON")
CPMAddPackage(NAME volk
              VERSION 1.3.270
              GIT_REPOSITORY https://github.com/zeux/volk.git
              GIT_TAG 1.3.270)
CPMAddPackage(NAME vulkanmemoryallocator
              VERSION 1.3.270
              GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
              GIT_TAG 33067f6b4f735db97c97660e46620266eebcf7de
              EXCLUDE_FROM_ALL TRUE
              OPTIONS "VMA_BUILD_DOCUMENTATION OFF"
                      "VMA_BUILD_SAMPLES OFF")
