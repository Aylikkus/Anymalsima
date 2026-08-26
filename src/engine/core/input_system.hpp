#pragma once
#include <vector>
#include <SDL3/SDL.h>

using Key = SDL_Scancode;

struct InputEvent {
    enum Type {
        MouseClick,
        KeyPress,
        KeyHold,
        TextInput,
        MouseMove,
        MouseWheel
    };

    enum ScrollDirection {
        Horizontal,
        Vertical
    };

    enum MouseButton {
        LEFT_BUTTON = 1,
        MIDDLE_BUTTON = 2,
        RIGHT_BUTTON = 3,
        X1_BUTTON = 4,
        X2_BUTTON = 5
    };

    Type type;
    bool pressed = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    int x = 0;
    int y = 0;

    Key key;
    MouseButton button;

    char16_t codepoint = 0;

    float delta = 0.f;
    int64_t time;
    ScrollDirection direction;
};

class InputSystem {
public:
    InputSystem();
    
    void add(const InputEvent& event);
    void clear();
    const std::vector<InputEvent>& getEvents() const;
private:
    std::vector<InputEvent> events;
};