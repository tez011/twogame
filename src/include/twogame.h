#pragma once
#include <array>
#include <iostream>
#include <list>
#include <memory>
#include <SDL.h>
#include <util.h>

namespace twogame {

class Renderer;
class Scene;

}

class Twogame {
private:
    SDL_Window* m_window;
    twogame::Renderer* m_renderer;
    twogame::Scene* m_current_scene;
    twogame::util::ThreadPool m_thread_pool;
    bool m_active;

    void initialize_filesystem(const char* argv0, const char* app_name);

public:
    Twogame(const char* argv0, const char* app_name);
    ~Twogame();

    inline twogame::Renderer* renderer() { return m_renderer; }
    inline twogame::util::ThreadPool& thread_pool() { return m_thread_pool; }

    void start();
};
