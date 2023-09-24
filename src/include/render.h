#pragma once
#include <cstdint>
#include <SDL.h>

class Twogame;

namespace twogame {

class Renderer {
protected:
    Twogame* m_twogame;
    SDL_Window* m_window;
    uint64_t m_frame_number;

public:
    Renderer(Twogame* tg, SDL_Window* window)
        : m_twogame(tg)
        , m_window(window)
        , m_frame_number(0)
    {
    }
    virtual ~Renderer() { }

    inline uint64_t frame_number() const { return m_frame_number; }

    virtual int32_t acquire_image() = 0;
    virtual void draw() = 0;
    virtual void next_frame(uint32_t image_index) = 0;
    virtual void wait_idle() = 0;
};

}