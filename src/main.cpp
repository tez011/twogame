#define SDL_MAIN_USE_CALLBACKS
#include <filesystem>
#include <iostream>
#include <cglm/cglm.h>
#include <ktx.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "asset.h"
#include "display.h"
#include "physfs.h"
#include "scene.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

class DuckScene : public twogame::IScene {
public:
    DuckScene();
    virtual ~DuckScene() { }

    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage) override;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage) override;
    virtual void render(twogame::IRenderer* renderer, uint32_t frame_number) override;
};

DuckScene::DuckScene()
{
    twogame::SceneManifest assets("/data/duck.tgs");
    m_assets += assets.assets();
    m_scenegraph = assets.scene();
    m_cameras.assign(assets.cameras().begin(), assets.cameras().end());
    m_draw_meshes.assign(assets.meshes().begin(), assets.meshes().end());

    // We need to do these sorting operations every time m_cameras or m_draw_meshes changes. But for now, they don't.
    std::sort(m_cameras.begin(), m_cameras.end(), [](const twogame::CameraNode& a, const twogame::CameraNode& b) {
        return a.camera_index < b.camera_index;
    });
    std::sort(m_draw_meshes.begin(), m_draw_meshes.end(), [](const twogame::MeshNode& a, const twogame::MeshNode& b) {
        return a.mesh_index < b.mesh_index;
    });
}

void DuckScene::handle_event(const SDL_Event& evt, twogame::SceneHost* stage)
{
}

void DuckScene::tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage)
{
    m_scenegraph.update_global_transforms();
}

void DuckScene::render(twogame::IRenderer* renderer, uint32_t frame_number)
{
    std::span instances = m_instances[frame_number % FRAMES_IN_FLIGHT];
    std::span draw_commands = m_draw_commands[frame_number % FRAMES_IN_FLIGHT];
    for (int ii = 0, di = -1, last_mesh; ii < m_draw_meshes.size(); ii++) {
        const twogame::MeshNode& drawable = m_draw_meshes[ii];
        instances[ii].model = m_scenegraph.global_transforms()[drawable.node_index];
        instances[ii].mesh_id = drawable.mesh_index;
        instances[ii].material_id = 0;
        if (ii == 0 || last_mesh != drawable.mesh_index) {
            ++di;
            draw_commands[di].vertexCount = m_assets.meshes()[drawable.mesh_index]->index_count();
            draw_commands[di].instanceCount = 1;
            draw_commands[di].firstVertex = 0;
            draw_commands[di].firstInstance = ii;
            last_mesh = drawable.mesh_index;
        } else {
            draw_commands[di].instanceCount++;
        }
    }

    m_binding_zero[frame_number % FRAMES_IN_FLIGHT].proj = renderer->projection();
    if (false && m_cameras.size() > 0) {
        m_binding_zero[frame_number % FRAMES_IN_FLIGHT].view = glms_mat4_inv_fast(m_scenegraph.global_transforms()[m_cameras.front().node_index]);
    } else {
        vec3s eye = { { 0, 2.5f, ((float)frame_number / 50.f) - 5.f } }, toward = { { 0, 1, 0 } }, up = { { 0, frame_number <= 250 ? 1.f : -1.f, 0 } };
        m_binding_zero[frame_number % FRAMES_IN_FLIGHT].view = glms_lookat(eye, toward, up);
    }
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
        twogame::SceneHost::init(new twogame::SimpleForwardRenderer, new DuckScene);
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
