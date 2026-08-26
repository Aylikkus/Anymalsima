#pragma once

#include "base_widget.hpp"
#include "core/geometry.hpp"

enum class ScrollAxis {
    Vertical,
    Horizontal,
    HorizontalAndVertical
};

class ScrollContainer : public BaseWidget {
public:
    void setAxis(ScrollAxis axis);
    void setScrollSpeed(float speed);
    void setScroll(float x, float y);
    Vec2 getScroll();
    Vec2 getContentSize();
    Vec2 getViewSize();

    virtual void updateLayout(const Rect& parentRect, const UIContext& context) override;
    virtual void draw(RenderTarget& target, const UIContext& context) override;
    virtual bool handleInput(const InputEvent& e, const UIContext& context) override;

private:
    Rect contentRect; // прямоугольник контента

    ScrollAxis axis = ScrollAxis::Vertical;

    float scrollY = 0.0f;
    float scrollX = 0.0f;

    float contentHeight = 0.0f;
    float contentWidth = 0.0f;
    float viewHeight = 0.0f;
    float viewWidth = 0.0f;

    float scrollSpeed = 30.0f;
};