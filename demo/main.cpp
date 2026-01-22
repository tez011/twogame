#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "shaders.h"
#include "twogame.h"
#include "twogame_vk.h"
#define APP_NAME "twogame demo"
#define ORG_NAME "tez011"
#define SHORT_APP_NAME "twogame_demo"
#define SHORT_ORG_NAME "tez011"

// fields that could be part of appstate
struct AppState {
    twogame::vk::DisplayHost* host;
    twogame::vk::SimpleForwardRenderer* renderer;

    AppState()
        : host(nullptr)
        , renderer(nullptr)
    {
    }
};

SDL_AppResult SDL_AppInit(void** _appstate, int argc, char** argv)
{
    static struct AppState appstate;
    *_appstate = &appstate;

    SDL_SetAppMetadata(APP_NAME, "0.0", "gh." SHORT_ORG_NAME "." SHORT_APP_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, ORG_NAME);
#ifdef DEBUG_BUILD
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#endif

    twogame::init();
#ifdef DEBUG_BUILD
    twogame::init_filesystem_debug(argv[0]);
#else
    twogame::init_filesystem(argv[0], SHORT_ORG_NAME, SHORT_APP_NAME);
#endif

    if ((appstate.host = twogame::vk::DisplayHost::create()) == nullptr)
        return SDL_APP_FAILURE;
    appstate.renderer = new twogame::vk::SimpleForwardRenderer(appstate.host);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* _appstate, SDL_Event* evt)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);

    switch (evt->type) {
    default:
        return appstate->host->handle_event(evt);
    }
}

SDL_AppResult SDL_AppIterate(void* _appstate)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    appstate->host->draw_frame(appstate->renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* _appstate, SDL_AppResult result)
{
    AppState* appstate = reinterpret_cast<AppState*>(_appstate);
    if (appstate->renderer)
        delete appstate->renderer;
    if (appstate->host)
        delete appstate->host;
    twogame::deinit();
}
