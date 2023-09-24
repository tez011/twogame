#pragma once
#include "render.h"
#include <array>
#include <iostream>
#include <list>
#include <memory>
#include <util.h>

class Twogame {
private:
    SDL_Window* m_window;
    twogame::Renderer* m_renderer;
    twogame::util::ThreadPool m_thread_pool;
    bool m_active;

    void initialize_filesystem(const char* argv0, const char* app_name);

public:
    Twogame(const char* argv0, const char* app_name);
    ~Twogame();

    inline twogame::util::ThreadPool& thread_pool() { return m_thread_pool; }

    void start();
};
