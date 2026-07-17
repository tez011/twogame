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
        twogame::SceneManifest assets("/d0/sponza_base.tgs");
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
};

static SDL_EnumerationResult find_resource_packs(void* ud, const char* dirname, const char* fname)
{
    static char fullpath[3072], mountpoint[1024];
    snprintf(fullpath, sizeof(fullpath), "%s%s", dirname, fname);
    SDL_PathInfo path_info;
    if (SDL_GetPathInfo(fullpath, &path_info) && (path_info.type == SDL_PATHTYPE_FILE || path_info.type == SDL_PATHTYPE_DIRECTORY)) {
        const char* ext = strrchr(fname, '.');
        if (ext != nullptr && strncasecmp(ext, ".pk2", 4) == 0) {
            snprintf(mountpoint, 1024, "/%.*s", static_cast<int>(ext - fname), fname);
            if (PHYSFS_mount(fullpath, mountpoint, 1) == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "failed to mount %s -> %s/: %s", fullpath, mountpoint, PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "mounted %s -> %s/", fullpath, mountpoint);
            }
        }
    }
    return SDL_ENUM_CONTINUE;
}

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

    const char* base_path = SDL_GetBasePath();
    if (PHYSFS_init(argv[0]) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "PHYSFS_init: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "searching for resource packs in %s", base_path);
    SDL_EnumerateDirectory(base_path, find_resource_packs, nullptr);

#ifdef DEBUG_BUILD
    if (PHYSFS_mount(TWOGAME_SOURCE_ROOT "/prefs", "/pref", 1) == 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "mount " TWOGAME_SOURCE_ROOT "/prefs -> /pref/: %s", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return SDL_APP_FAILURE;
    }
    PHYSFS_setWriteDir(TWOGAME_SOURCE_ROOT "/prefs");
#else
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
