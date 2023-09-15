#pragma once
#include "render.h"
#include <array>
#include <iostream>
#include <list>
#include <memory>

class Twogame {
private:
    SDL_Window* m_window;
    twogame::Renderer* m_renderer;

    void initialize_filesystem(const char* argv0, const char* app_name);

public:
    Twogame(const char* argv0, const char* app_name);
    ~Twogame();
};
