#define SDL_MAIN_USE_CALLBACKS
#include <physfs.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "render/display_host.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "scene/scene_host.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

class DemoScene : public twogame::IScene {
public:
    DemoScene()
    {
        twogame::SceneManifest assets("/data/fox.tgs");
        m_assets += assets.assets();
        m_scenegraph = assets.scene();
        m_cameras.assign(assets.cameras().begin(), assets.cameras().end());
        m_draw_meshes.assign(assets.meshes().begin(), assets.meshes().end());
        m_sparse_meshes.assign(m_scenegraph.node_count(), std::numeric_limits<uint32_t>::max());
    }
    virtual ~DemoScene() { }

    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage) override
    {
        if (evt.type == SDL_EVENT_KEY_UP && evt.key.key >= SDLK_1 && evt.key.key <= SDLK_9) {
            unsigned int new_animation = evt.key.key - SDLK_1;
            if (new_animation < m_assets.animations().size()) {
                if (m_animations.empty())
                    m_animations.emplace_back();

                m_animations.back().start_time = SDL_GetTicks();
                m_animations.back().animation_index = new_animation;
                m_animations.back().keyframe_hints = std::make_unique<uint32_t[]>(m_assets.animations().at(new_animation)->total_samplers());
                m_animations.back().custom_targets = m_draw_meshes.front().skin;
                m_animations.back().loop = true;
            } else {
                m_animations.clear();
            }
        }
    }

    virtual void logic(uint64_t frame_number, uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage) override
    {
        if (m_cameras.empty()) {
#if 1
            vec3s eye = { { 100.f,
                200.f,
                150.f } },
                  toward = { { 0,
                      50.f,
                      0 } },
                  up = { { 0,
                      1.f,
                      0 } };
#else
            vec3s eye = { { 0.f,
                      2.f,
                      3.f } },
                  toward = { { 0,
                      1.f,
                      0 } },
                  up = { { 0,
                      1.f,
                      0 } }; // Bars/Cube
#endif
            m_binding_zero[frame_number % FRAMES_IN_FLIGHT].view = glms_lookat(eye, toward, up);
        }
    }
};

SDL_AppResult SDL_AppInit(void** _appstate, int argc, char** argv)
{
    SDL_SetAppMetadata(APP_NAME, "0.0", "gh." SHORT_ORG_NAME "." SHORT_APP_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, ORG_NAME);
#ifdef DEBUG_BUILD
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif
    Uint32 init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_SENSOR;
    if (volkInitialize() != VK_SUCCESS) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "volkInitialize: no loader found");
        return SDL_APP_FAILURE;
    }
    if (SDL_Init(init_flags) == false) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

#ifdef DEBUG_BUILD
    constexpr const char* rsrc_root = TWOGAME_SOURCE_ROOT "/resources";
    constexpr const char* pref_root = TWOGAME_SOURCE_ROOT "/prefs";
    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(rsrc_root, "/data", 0) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /data/: %s", rsrc_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    if (PHYSFS_mount(pref_root, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount %s -> /pref/: %s", pref_root, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    PHYSFS_setWriteDir(pref_root);
#else
    twogame::init_filesystem(argv[0], SHORT_ORG_NAME, SHORT_APP_NAME);
    char mountpoint[4096];
    const char* base_path = SDL_GetBasePath();

    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    for (const auto& dirent : std::filesystem::directory_iterator(base_path)) {
        if (dirent.is_regular_file() == false && dirent.is_directory() == false)
            continue;

        const auto& path = dirent.path();
        if (path.has_filename() && path.has_stem() && strncasecmp(path.extension().c_str(), ".pk2", 4) == 0) {
            const char* fullpath = path.c_str();
            snprintf(mountpoint, 4096, "/%s", path.stem().c_str());
            if (PHYSFS_mount(fullpath, mountpoint, 1) == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> %s/: %s", fullpath, mountpoint, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> %s/", fullpath, mountpoint);
            }
        }
    }

    char* pref_path = SDL_GetPrefPath(SHORT_ORG_NAME, SHORT_APP_NAME);
    if (PHYSFS_mount(pref_path, "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> /pref/: %s", pref_path, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> /pref/", pref_path);
    }
    PHYSFS_setWriteDir(pref_path);
    SDL_free(pref_path);
#endif

#ifndef DEBUG_BUILD
    try {
#endif
        twogame::DisplayHost::init();
        twogame::SceneHost::init(new twogame::SimpleForwardRenderer, new DemoScene);
#ifndef DEBUG_BUILD
    } catch (...) {
        return SDL_APP_FAILURE;
    }
#endif

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    if (evt->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    twogame::SceneHost::push_event(evt);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    if (twogame::DisplayHost::owned().draw_frame(twogame::SceneHost::renderer()) < 0)
        return SDL_APP_FAILURE;

    twogame::SceneHost::submit_transfers();
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    twogame::SceneHost::drop();
    twogame::DisplayHost::drop();
    if (PHYSFS_isInit())
        PHYSFS_deinit();
    SDL_Quit();
}
