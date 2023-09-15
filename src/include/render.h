#pragma once
#include <cstdint>
#include <SDL.h>

namespace twogame {

class Renderer {
protected:
    SDL_Window* m_window;

public:
    Renderer(SDL_Window* window)
        : m_window(window)
    {
    }
    virtual ~Renderer() { }
};

}