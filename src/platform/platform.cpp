#include "platform.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>

Platform::Platform(int w, int h) : width(w), height(h) {
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow("Anymalsima", w, h, SDL_WINDOW_RESIZABLE);
    assert(window);
}

Platform::~Platform() {
    if (window) {
        SDL_DestroyWindow(window);
    }

    SDL_Quit();
}

bool Platform::isRunning() const { return running; }

void Platform::applyModifiers(InputEvent& ev, SDL_Keymod& mod) {
    ev.shift = (mod & SDL_KMOD_SHIFT) != 0;
    ev.ctrl = (mod & SDL_KMOD_CTRL) != 0;
    ev.alt = (mod & SDL_KMOD_ALT) != 0;
}

void Platform::pollEvents(InputSystem& input) {
    SDL_Event e;
    SDL_Keymod mod = SDL_GetModState();

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                onClose.emit();
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                onClose.emit();
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                width = e.window.data1;
                height = e.window.data2;

                onResize.emit({(float)width, (float)height});
                break;
            }

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                InputEvent ev{};
                ev.type = InputEvent::KeyPress;
                ev.key = e.key.scancode;
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    ev.pressed = true;
                    pressed_keys.insert(static_cast<uint8_t>(ev.key));
                }
                else {
                    ev.pressed = false;
                    pressed_keys.erase(static_cast<uint8_t>(ev.key));
                }
                ev.time = getTimeMs();

                const SDL_Keymod& mod = e.key.mod;

                ev.shift = (mod & SDL_KMOD_SHIFT) != 0;
                ev.ctrl = (mod & SDL_KMOD_CTRL) != 0;
                ev.alt = (mod & SDL_KMOD_ALT) != 0;

                input.add(ev);
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                InputEvent ev{};
                ev.type = InputEvent::MouseClick;
                ev.pressed = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                ev.x = e.button.x;
                ev.y = e.button.y;
                ev.button =
                    static_cast<InputEvent::MouseButton>(e.button.button);
                ev.time = getTimeMs();

                applyModifiers(ev, mod);

                input.add(ev);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION: {
                InputEvent ev{};
                ev.type = InputEvent::MouseMove;
                ev.x = e.motion.x;
                ev.y = e.motion.y;
                ev.time = getTimeMs();

                applyModifiers(ev, mod);

                input.add(ev);
                break;
            }

            case SDL_EVENT_MOUSE_WHEEL: {
                InputEvent ev{};
                ev.type = InputEvent::MouseWheel;
                if (e.wheel.x != 0) {
                    ev.delta = e.wheel.x;
                    ev.direction = InputEvent::ScrollDirection::Horizontal;
                }
                else {
                    ev.delta = e.wheel.y;
                    ev.direction = InputEvent::ScrollDirection::Vertical;
                }
                ev.x = e.wheel.mouse_x;
                ev.y = e.wheel.mouse_y;
                ev.time = getTimeMs();

                applyModifiers(ev, mod);

                input.add(ev);
                break;
            }

            case SDL_EVENT_TEXT_INPUT: {
                InputEvent ev{};
                ev.type = InputEvent::TextInput;
                ev.codepoint = (char16_t)e.text.text[0];
                ev.time = getTimeMs();

                input.add(ev);
                break;
            }
        }
    }

    for (uint8_t k : pressed_keys) {
        InputEvent ev{};
        ev.type = InputEvent::KeyHold;
        ev.key = static_cast<Key>(k);
        applyModifiers(ev, mod);

        input.add(ev);
    }
}

double Platform::getFps() { 
    // 1. Get the stats from the last frame
    const bgfx::Stats* stats = bgfx::getStats();

    // 2. Calculate the frame time (in milliseconds)
    // stats->cpuTimerFreq is the frequency of the timer
    double frameTimeMs = (double)stats->cpuTimeFrame / (double)stats->cpuTimerFreq * 1000.0;

    // 3. Calculate FPS
    return (double)stats->cpuTimerFreq / (double)stats->cpuTimeFrame;
}

float Platform::deltaTime() {
    Uint32 curTick = SDL_GetTicks();
    if (frameMinMs > 0) {
        Uint32 elapsed = curTick - lastTick;
        if (elapsed < frameMinMs)
            SDL_Delay(frameMinMs - elapsed);
        curTick = SDL_GetTicks();
    }
    float dt = (curTick - lastTick) / 1000.0f;
    lastTick = curTick;
    return dt;
}

bgfx::PlatformData Platform::getPlatformData() {
    bgfx::PlatformData pd{};
    if (!window) return pd;
    SDL_PropertiesID props = SDL_GetWindowProperties(window);

#if defined(SDL_PLATFORM_WIN32)
    pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                    nullptr);

#elif defined(SDL_PLATFORM_LINUX)
    const char* driver = SDL_GetCurrentVideoDriver();

    if (SDL_strcmp(driver, "x11") == 0) {
        pd.ndt = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        // X11 Window IDs are numeric, so use GetNumberProperty and cast
        pd.nwh = (void*)(uintptr_t)SDL_GetNumberProperty(
            props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(driver, "wayland") == 0) {
        pd.type = bgfx::NativeWindowHandleType::Wayland;
        pd.ndt = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        pd.nwh = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    }

#elif defined(SDL_PLATFORM_APPLE)  // Covers macOS and iOS
    pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                    nullptr);
#endif

    pd.context = nullptr;
    pd.backBuffer = nullptr;
    pd.backBufferDS = nullptr;

    return pd;
}

SDL_Window* Platform::getWindow() { return window; }

int64_t Platform::getTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

Vec2 Platform::getWindowSize() { return {(float)width, (float)height}; }

void Platform::stop() { running = false; }

void Platform::close() {
    if (window) SDL_DestroyWindow(window);

    window = nullptr;
}

void Platform::setFpsLimit(int fps) {
    if (fps == 0) {
        frameMinMs = 0;
        return;
    }
    frameMinMs = 1000 / fps;
}
