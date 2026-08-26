#pragma once

#include "base_widget.hpp"
#include "scroll_container.hpp"
#include "core/geometry.hpp"

class ScrollBar : public BaseWidget {
public:
    ScrollBar(const UIContext& context, const std::string& trackTexture, const std::string& thumbTexture);

    void setNormalColor(Color c);
    void setHoverColor(Color c);
    void setTarget(ScrollContainer* t);
    void setAxis(ScrollAxis axis);
    void setTrackSize(float start, float end);
    void setThumbSize(float start, float end);

    virtual void updateLayout(const Rect&, const UIContext&) override;
    virtual void draw(RenderTarget&, const UIContext&) override;
    virtual bool handleInput(const InputEvent&, const UIContext&) override;

private:
    Rect thumbRect;
    ScrollAxis axis = ScrollAxis::Vertical;
    ScrollContainer* target = nullptr;

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

    float getValue() const;      // 0..1
    float getThumbSize() const;  // 0..1
    void drawTrack(RenderTarget& targetRT, ScrollAxis a);
    void drawThumb(RenderTarget& targetRT, ScrollAxis a);
    ScrollAxis effectiveAxis() const;
};