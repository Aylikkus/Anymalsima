#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_properties.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <unordered_set>

#include "core/geometry.hpp"
#include "core/input_system.hpp"
#include "core/platform_api.hpp"
#include "core/signal.hpp"

class Platform : public IPlatform {
   public:
    Platform(int w = 1280, int h = 720);
    ~Platform();

    bool isRunning() const;
    void pollEvents(InputSystem& input);

    void setFpsLimit(int fps);

    void stop() override;
    void close();

    int64_t getTimeMs() override;
    double getFps() override;
    Vec2 getWindowSize();
    float deltaTime();
    bgfx::PlatformData getPlatformData();

    Signal<Vec2> onResize;
    Signal<> onClose;

    SDL_Window* getWindow();

   private:
    float frameMinMs = 1000.0f / 60.0f;
    bool running = true;

    SDL_Window* window = nullptr;

    uint32_t width;
    uint32_t height;

    Uint32 lastTick = SDL_GetTicks();
    std::unordered_set<uint16_t> pressed_keys;

    void applyModifiers(InputEvent& ev, SDL_Keymod& mod);
};