#include "twogame.h"
#include "vk.h"
#include <cassert>
#include <exception>
#include <filesystem>
#include <physfs.h>
#include <SDL.h>

static bool SDL_WAS_INITTED = false;

Twogame::Twogame(const char* argv0, const char* app_name)
{
    if (SDL_WAS_INITTED) {
        if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s\n", SDL_GetError());
            std::terminate();
        }

        SDL_WAS_INITTED = true;
    }

    initialize_filesystem(argv0, app_name);
    if ((m_window = SDL_CreateWindow(app_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)) == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow: %s\n", SDL_GetError());
        std::terminate();
    }

    m_renderer = new twogame::VulkanRenderer(m_window);
}

Twogame::~Twogame()
{
    delete m_renderer;
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Twogame::initialize_filesystem(const char* argv0, const char* app_name)
{
    if (PHYSFS_init(argv0) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }

#ifdef TWOGAME_DEBUG_BUILD
    constexpr const char* rsrc_root = TWOGAME_SOURCE_ROOT "/resources";
    constexpr const char* pref_root = TWOGAME_SOURCE_ROOT "/prefs";

    (void)app_name;
    if (PHYSFS_mount(rsrc_root, "/tg", 0) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> /tg/: %s", rsrc_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }
    if (PHYSFS_mount(pref_root, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> /pref/: %s", pref_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }
    PHYSFS_setWriteDir(pref_root);
#else
    char* base_path = SDL_GetBasePath();
    for (const auto& dirent : std::filesystem::directory_iterator(base_path)) {
        if (dirent.is_regular_file() == false && dirent.is_directory() == false)
            continue;

        const auto& path = dirent.path();
        if (path.has_filename() && path.has_stem() && strncasecmp(path.extension().c_str(), ".pk2", 4) == 0) {
            const char* fullpath = path.c_str();
            std::string mountpoint = "/tg/" + path.stem().generic_string();
            if (PHYSFS_mount(fullpath, mountpoint.c_str(), 1) == 0) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> %s/: %s", fullpath, mountpoint.c_str(), PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
                std::terminate();
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> %s/", fullpath, mountpoint.c_str());
        }
    }
    SDL_free(base_path);

    char* pref_path = SDL_GetPrefPath("twogame", app_name);
    if (PHYSFS_mount(pref_path, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> /pref/: %s", pref_path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mount %s -> /pref/", pref_path);
    PHYSFS_setWriteDir(pref_path);
    SDL_free(pref_path);
#endif
}

void Twogame::start()
{
    Uint64 frame0_time = SDL_GetTicks64(), last_frame_time = frame0_time, frame_time = SDL_GetTicks64();
    m_active = true;

    while (m_active) {
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) {
                m_active = false;
            } else if (evt.type == SDL_WINDOWEVENT) {
                if (evt.window.event == SDL_WINDOWEVENT_HIDDEN) {
                    while (evt.window.event != SDL_WINDOWEVENT_EXPOSED)
                        SDL_WaitEvent(&evt);
                }
                if (evt.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    while (evt.window.event != SDL_WINDOWEVENT_RESTORED)
                        SDL_WaitEvent(&evt);
                }
            }

            // logic(frame_time - last_frame_time)

            int image_index = m_renderer->acquire_image();
            if (image_index < 0) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "failed to acquire image before rendering");
                m_active = false;
                break;
            }
            last_frame_time = frame_time;
            frame_time = SDL_GetTicks64();

            m_renderer->draw();
            m_renderer->next_frame(image_index);
        }
        m_renderer->wait_idle();
    }
}
