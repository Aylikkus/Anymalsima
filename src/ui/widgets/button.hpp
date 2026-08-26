#pragma once

#include <functional>
#include "core/input_system.hpp"
#include "base_widget.hpp"
#include "audio/audio_system_api.hpp"
#include <random>

class Button : public BaseWidget {
public:
    Button(const UIContext& context, const std::string& texture, const std::string& click_sound);

    Signal<> onClick;
    Signal<> onHover;
    Signal<> onPressed;
    Signal<> onReleased;

    void setNormalColor(Color c);
    void setHoverColor(Color c);
    void setPressedColor(Color c);
    Color getNormalColor();
    Color getHoverColor();
    Color getPressedColor();
    void setSliceBorders(float l, float r, float t, float b);
    
    virtual bool handleInput(const InputEvent& e, const UIContext& context) override;
    virtual void draw(RenderTarget& target, const UIContext& context) override;
private:
    bool pressed = false;
    
    float left = 16, right = 16, top = 16, bottom = 16;
   

    std::string texture;
    std::string click_sound;

    Vec2u texture_size;

    Color normalTint = {232,232,232,255};
    Color hoverTint = {255,255,255,255};
    Color pressedTint = {192,192,192,255};

    std::mt19937 gen;
    std::uniform_real_distribution<float> pitch_dis;
};