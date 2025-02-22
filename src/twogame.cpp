#include "twogame.h"
#include <cassert>
#include <exception>
#include <filesystem>
#include <physfs.h>
#include <SDL.h>
#include <spdlog/spdlog.h>
#include "render.h"
#include "scene.h"

static bool SDL_WAS_INITTED = false;

Twogame::Twogame(const char* argv0, const char* app_name, const char* scene)
{
    if (SDL_WAS_INITTED == false) {
        if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
            spdlog::critical("SDL_Init: {}", SDL_GetError());
            std::terminate();
        }
        if (volkInitialize() != VK_SUCCESS) {
            spdlog::critical("volkInitialize: no loader found");
            std::terminate();
        }

        SDL_WAS_INITTED = true;
    }

    initialize_filesystem(argv0, app_name);
    if ((m_window = SDL_CreateWindow(app_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)) == nullptr) {
        spdlog::critical("SDL_CreateWindow: {}\n", SDL_GetError());
        std::terminate();
    }

    m_renderer = new twogame::Renderer(this, m_window);
    m_current_scene = new twogame::Scene(this, scene ? scene : "/tg/scene.xml");
}

Twogame::~Twogame()
{
    delete m_current_scene;
    delete m_renderer;
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Twogame::initialize_filesystem(const char* argv0, const char* app_name)
{
    if (PHYSFS_init(argv0) == 0) {
        spdlog::critical("PHYSFS_init: {}", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }

#ifdef TWOGAME_DEBUG_BUILD
    constexpr const char* rsrc_root = TWOGAME_SOURCE_ROOT "/resources";
    constexpr const char* pref_root = TWOGAME_SOURCE_ROOT "/prefs";

    (void)app_name;
    if (PHYSFS_mount(rsrc_root, "/tg", 0) == 0) {
        spdlog::critical("mount {} -> /tg/: {}", rsrc_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }
    if (PHYSFS_mount(pref_root, "/pref", 1) == 0) {
        spdlog::critical("mount {} -> /pref/: {}", pref_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
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
                spdlog::critical("mount {} -> {}/: {}", fullpath, mountpoint.c_str(), PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
                std::terminate();
            }
            spdlog::info("mount {} -> {}/", fullpath, mountpoint.c_str());
        }
    }
    SDL_free(base_path);

    char* pref_path = SDL_GetPrefPath("twogame", app_name);
    if (PHYSFS_mount(pref_path, "/pref", 1) == 0) {
        spdlog::critical("mount {} -> /pref/: {}", pref_path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        std::terminate();
    }
    spdlog::info("mount {} -> /pref/", pref_path);
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
        }

        m_current_scene->animate(frame_time, frame_time - last_frame_time);
        m_current_scene->update_transforms();
        m_current_scene->update_perobject_descriptors();

        int image_index = m_renderer->acquire_image();
        if (image_index < 0) {
            spdlog::critical("failed to acquire image before rendering");
            m_active = false;
            break;
        }
        last_frame_time = frame_time;
        frame_time = SDL_GetTicks64();

        m_renderer->draw(m_current_scene);
        m_renderer->next_frame(image_index);
        m_renderer->wait_idle();
    }
}
