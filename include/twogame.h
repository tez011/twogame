#pragma once
#include <SDL3/SDL.h>

namespace twogame {

bool init();
bool init_filesystem(const char* argv0, const char* org_name, const char* app_name);
bool init_filesystem_debug(const char* argv0);
void deinit();

}
