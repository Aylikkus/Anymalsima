#pragma once

#include "base_widget.hpp"
#include "scroll_container.hpp"
#include "core/geometry.hpp"

class Slider : public BaseWidget {
public:
    Slider(const UIContext& context, const std::string& trackTexture, const std::string& thumbTexture);

    Signal<float> onValueChanged;

    void setRange(float min, float max);

    void setStep(float s);

    void setValue(float v);

    float getValue() const;

    void setNormalColor(Color c);
    void setHoverColor(Color c);
    void setAxis(ScrollAxis axis);
    void setTrackSize(float start, float end);
    void setThumbSize(float start, float end);
    void setThumbLength(float px);

    virtual void updateLayout(const Rect&, const UIContext&) override;
    virtual void draw(RenderTarget&, const UIContext&) override;
    virtual bool handleInput(const InputEvent&, const UIContext&) override;

private:
    Rect thumbRect;
    ScrollAxis axis = ScrollAxis::Vertical;

    std::string trackTexture; // фон
    std::string thumbTexture; // ползунок

    Vec2u trackTextureSize;
    Vec2u thumbTextureSize;

    Color normalTint = {224, 224, 224, 255};
    Color hoverTint = {255, 255, 255, 255};

    float trackStartSize = 16;
    float trackEndSize = 16;

    float thumbStartSize = 16;
    float thumbEndSize = 16;

    bool dragging = false;
    bool hover = false;
    float dragOffset = 0;

    float minValue = 0.0f;
    float maxValue = 1.0f;
    float value = 0.0f;
    float step = 0.0f;

    float thumbLength = 30.0f;

    void drawTrack(RenderTarget& targetRT, ScrollAxis a);
    void drawThumb(RenderTarget& targetRT, ScrollAxis a);
    float getNormalized() const;
    ScrollAxis effectiveAxis() const;
};