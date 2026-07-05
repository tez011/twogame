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

class DemoScene : public twogame::IScene {
public:
    DemoScene();
    virtual ~DemoScene() { }

    virtual void handle_event(const SDL_Event& evt, twogame::SceneHost* stage) override;
    virtual void tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage) override;
    virtual void render(twogame::IRenderer* renderer, uint32_t frame_number) override;
};

DemoScene::DemoScene()
{
    twogame::SceneManifest assets("/data/fox.tgs");
    m_assets += assets.assets();
    m_scenegraph = assets.scene();
    m_cameras.assign(assets.cameras().begin(), assets.cameras().end());
    m_draw_meshes.assign(assets.meshes().begin(), assets.meshes().end());
    m_sparse_meshes.assign(m_scenegraph.node_count(), std::numeric_limits<uint32_t>::max());

    // We need to do these sorting operations every time m_cameras or m_draw_meshes changes. But for now, they don't.
    std::fill(m_sparse_meshes.begin(), m_sparse_meshes.end(), std::numeric_limits<uint32_t>::max());
    std::sort(m_cameras.begin(), m_cameras.end(), [](const twogame::CameraNode& a, const twogame::CameraNode& b) {
        return a.camera_index < b.camera_index;
    });
    std::sort(m_draw_meshes.begin(), m_draw_meshes.end(), [this](const twogame::MeshNode& a, const twogame::MeshNode& b) {
        const auto ma = m_assets.meshes()[a.mesh_index], mb = m_assets.meshes()[b.mesh_index];
        if (ma->pipeline_key() != mb->pipeline_key())
            return ma->pipeline_key() < mb->pipeline_key();
        return a.mesh_index < b.mesh_index;
    });
    for (size_t i = 0; i < m_draw_meshes.size(); i++)
        m_sparse_meshes[m_draw_meshes[i].node_index] = i;
}

void DemoScene::handle_event(const SDL_Event& evt, twogame::SceneHost* stage)
{
    if (evt.type == SDL_EVENT_KEY_UP && evt.key.key >= SDLK_1 && evt.key.key <= SDLK_9) {
        int new_animation = evt.key.key - SDLK_1;
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

void DemoScene::tick(uint64_t frame_time, uint64_t delta_time, twogame::SceneHost* stage)
{
    static std::vector<vec4s> animation_data;
    for (auto it = m_animations.begin(); it != m_animations.end(); ++it) {
        const std::shared_ptr<twogame::asset::Animation>& anim = m_assets.animations().at(it->animation_index);
        int64_t anim_time = frame_time - it->start_time;
        if (anim_time < 0)
            continue;
        else if (anim_time > anim->duration() && it->loop)
            anim_time %= anim->duration();
        animation_data.resize(anim->keyframe_width());
        anim->interpolate(anim_time / 1000.f, (vec4*)animation_data.data(), std::span(it->keyframe_hints.get(), anim->total_samplers()));

        for (size_t i = 0, off = 0; i < anim->targets().size(); i++) {
            uint32_t object = anim->targets()[i].object;
            if (const auto* ct = std::get_if<std::unique_ptr<uint32_t[]>>(&it->custom_targets)) {
                if ((*ct)[i] != std::numeric_limits<uint32_t>::max())
                    object = (*ct)[i];
            } else if (anim->targets()[i].object_is_bone) {
                object = std::get<std::weak_ptr<uint32_t[]>>(it->custom_targets).lock()[object];
            }
            switch (anim->targets()[i].field) {
            case twogame::AnimationTarget::Field::Translation:
                m_scenegraph.transform(object).translation = glms_vec4_copy3(animation_data[off++]);
                break;
            case twogame::AnimationTarget::Field::Rotation:
                memcpy(m_scenegraph.transform(object).rotation.raw, animation_data[off++].raw, sizeof(versor));
                break;
            case twogame::AnimationTarget::Field::Scale:
                m_scenegraph.transform(object).scale = glms_vec4_copy3(animation_data[off++]);
                break;
            case twogame::AnimationTarget::Field::Weights:
                if (m_sparse_meshes[object] != std::numeric_limits<uint32_t>::max())
                    memcpy(m_draw_meshes[m_sparse_meshes[object]].weights.get(), animation_data[off].raw,
                        m_assets.meshes().at(m_draw_meshes[m_sparse_meshes[object]].mesh_index)->displacement_count() * sizeof(float));
                break;
            }
        }
    }

    m_scenegraph.update_global_transforms();
}

void DemoScene::render(twogame::IRenderer* renderer, uint32_t frame_number)
{
    std::span varying = m_varying[frame_number % FRAMES_IN_FLIGHT];
    std::span instances = m_instances[frame_number % FRAMES_IN_FLIGHT];
    std::span draw_commands = m_draw_commands[frame_number % FRAMES_IN_FLIGHT];
    VkDeviceAddress varying_buffer_address = 0;
    if (varying.empty() == false) {
        VkBufferDeviceAddressInfo bda_info {};
        bda_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bda_info.buffer = m_varying_buffer[frame_number % FRAMES_IN_FLIGHT].handle;
        varying_buffer_address = vkGetBufferDeviceAddress(twogame::DisplayHost::device(), &bda_info);
    }

    size_t i = 0, di = 0, v_tail = 0;
    for (i = 0; i < m_draw_meshes.size(); i++) {
        const twogame::MeshNode& drawable = m_draw_meshes[i];
        instances[i].model = m_scenegraph.global_transforms()[drawable.node_index];
        instances[i].mesh_id = drawable.mesh_index;
        instances[i].material_id = drawable.material_index;
        if (drawable.skeleton_index != std::numeric_limits<uint32_t>::max()) {
            mat4 inverse_mesh_xfm, relative_joint_xfm;
            const auto& skeleton = m_assets.skeletons()[drawable.skeleton_index];
            std::span<mat4s> joint_matrices = std::span(reinterpret_cast<mat4s*>(varying.subspan(v_tail).data()), skeleton->joints().size());
            instances[i].joint_matrices = varying_buffer_address + v_tail * sizeof(vec4);
            glm_mat4_inv(instances[i].model.raw, inverse_mesh_xfm);
            for (size_t j = 0; j < skeleton->joints().size(); j++) {
                glm_mat4_mul(inverse_mesh_xfm, const_cast<vec4*>(m_scenegraph.global_transforms()[drawable.skin[j]].raw), relative_joint_xfm);
                glm_mat4_mul(relative_joint_xfm, const_cast<vec4*>(skeleton->skin_matrices()[j].raw), joint_matrices[j].raw);
            }
            v_tail += 4 * skeleton->joints().size();
        } else {
            instances[i].joint_matrices = 0;
        }
    }
    for (i = 0; i < m_draw_meshes.size(); i++) {
        if (m_draw_meshes[i].weights) {
            size_t count = m_assets.meshes()[m_draw_meshes[i].mesh_index]->displacement_count();
            instances[i].morph_weights = varying_buffer_address + v_tail * sizeof(vec4);
            memcpy(varying.subspan(v_tail).data(), m_draw_meshes[i].weights.get(), count * sizeof(float));
            v_tail += (count + 3) >> 2;
        } else {
            instances[i].morph_weights = 0;
        }
    }

    size_t last_mesh = 0;
    memset(draw_commands.data(), 0, sizeof(VkDrawIndirectCommand));
    for (i = 0, di = 0; i < m_draw_meshes.size(); i++) {
        if (last_mesh != m_draw_meshes[i].mesh_index) {
            ++di;
            draw_commands[di].instanceCount = 0;
            draw_commands[di].firstVertex = 0;
            draw_commands[di].firstInstance = i;
        }
        draw_commands[di].vertexCount = m_assets.meshes()[m_draw_meshes[i].mesh_index]->index_count();
        draw_commands[di].instanceCount++;
        last_mesh = m_draw_meshes[i].mesh_index;
    }
    memset(draw_commands.subspan(di + 1).data(), 0, sizeof(VkDrawIndirectCommand) * (m_draw_meshes.size() - (di + 1)));

    m_binding_zero[frame_number % FRAMES_IN_FLIGHT].proj = renderer->projection();
    if (m_cameras.size() > 0) {
        m_binding_zero[frame_number % FRAMES_IN_FLIGHT].view = glms_mat4_inv_fast(m_scenegraph.global_transforms()[m_cameras.front().node_index]);
    } else {
#if 1
        vec3s eye = { { -100.f, 100.f, 150.f } }, toward = { { 0, 50.f, 0 } }, up = { { 0, 1.f, 0 } }; // Fox
#else
        vec3s eye = { { 0.f, 2.f, 3.f } }, toward = { { 0, 1.f, 0 } }, up = { { 0, 1.f, 0 } }; // Bars/Cube
#endif
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
