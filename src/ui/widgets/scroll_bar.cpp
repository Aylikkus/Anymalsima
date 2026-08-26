#include "scroll_bar.hpp"
#include <assert.h>

ScrollBar::ScrollBar(const UIContext& context, const std::string& trackTexture, const std::string& thumbTexture)
{
    this->trackTexture = trackTexture;
    this->thumbTexture = thumbTexture;
    trackTextureSize = context.assets->getTextureSize(trackTexture);
    thumbTextureSize = context.assets->getTextureSize(thumbTexture);
}

void ScrollBar::updateLayout(const Rect &parentRect, const UIContext &context)
{
    BaseWidget::updateLayout(parentRect, context);

    float value = getValue();
    float size = getThumbSize();

    ScrollAxis a = effectiveAxis();

    if (a == ScrollAxis::Vertical)
    {
        float track = computedRect.height;
        float thumbH = track * size;

        float y = computedRect.y + value * (track - thumbH);

        thumbRect = Rect::makeRect(
            computedRect.x,
            y,
            computedRect.width,
            thumbH);
    }
    else
    {
        float track = computedRect.width;
        float thumbW = track * size;

        float x = computedRect.x + value * (track - thumbW);

        thumbRect = Rect::makeRect(
            x,
            computedRect.y,
            thumbW,
            computedRect.height);
    }
}

bool ScrollBar::handleInput(const InputEvent &e, const UIContext &)
{
    if (!enabled || !visible || !target)
        return false;

    ScrollAxis a = effectiveAxis();

    if (e.type == InputEvent::MouseClick && e.pressed)
    {
        if (thumbRect.contains(e.x, e.y))
        {
            dragging = true;

            if (a == ScrollAxis::Vertical)
                dragOffset = e.y - thumbRect.y;
            else
                dragOffset = e.x - thumbRect.x;

            return true;
        }
    }

    if (e.type == InputEvent::MouseMove ) {
        bool newHover = hover;
        if (thumbRect.contains(e.x, e.y))
        {
            newHover = true;
        }
        else {
            newHover = false;
        }
        if (newHover != hover) {
            hover = newHover;
        }
    }

    if (e.type == InputEvent::MouseMove && dragging)
    {
        float value;

        if (a == ScrollAxis::Vertical)
        {
            float track = computedRect.height;
            float thumbH = thumbRect.height;

            float y = e.y - dragOffset;
            value = (y - computedRect.y) / (track - thumbH);
        }
        else
        {
            float track = computedRect.width;
            float thumbW = thumbRect.width;

            float x = e.x - dragOffset;
            value = (x - computedRect.x) / (track - thumbW);
        }

        value = std::clamp(value, 0.0f, 1.0f);

        Vec2 contentSize = target->getContentSize();
        Vec2 viewSize = target->getViewSize();
        Vec2 scroll = target->getScroll();

        if (a == ScrollAxis::Vertical)
        {
            float maxScroll = std::max(0.0f, contentSize.y - viewSize.y);
            target->setScroll(scroll.x, value * maxScroll);
        }
        else
        {
            float maxScroll = std::max(0.0f, contentSize.x - viewSize.x);
            target->setScroll(value * maxScroll, scroll.y);
        }

        return true;
    }

    if (e.type == InputEvent::MouseClick && !e.pressed)
    {
        dragging = false;
    }

    return false;
}

float ScrollBar::getValue() const
{
    if (!target)
        return 0.0f;

    ScrollAxis a = effectiveAxis();

    Vec2 contentSize = target->getContentSize();
    Vec2 viewSize = target->getViewSize();
    Vec2 scroll = target->getScroll();

    if (a == ScrollAxis::Vertical)
    {
        float maxScroll = std::max(0.0f, contentSize.y - viewSize.y);
        if (maxScroll <= 0.0f)
            return 0.0f;
        return scroll.y / maxScroll;
    }
    else
    {
        float maxScroll = std::max(0.0f, contentSize.x - viewSize.x);
        if (maxScroll <= 0.0f)
            return 0.0f;
        return scroll.x / maxScroll;
    }
}

void ScrollBar::draw(RenderTarget &targetRT, const UIContext &context)
{
    if (!enabled || !visible)
        return;

    if (getThumbSize() >= 0.999f)
        return; // можно вообще не рисовать scrollbar

    ScrollAxis a = effectiveAxis();

    // 1. TRACK
    drawTrack(targetRT, a);

    // 2. THUMB
    drawThumb(targetRT, a);
}

void ScrollBar::drawTrack(RenderTarget &targetRT, ScrollAxis a)
{
    const Rect &r = computedRect;

    const Vec2u& texSize = trackTextureSize;

    if (a == ScrollAxis::Vertical)
    {
        float top = trackStartSize;
        float bottom = trackEndSize;

        float centerH = r.height - top - bottom;


        float u0 = 0;
        float u1 = texSize.x;

        float v0 = 0;
        float v1 = top;
        float v2 = texSize.y - bottom;

        // TOP
        targetRT.drawSprite(
            Rect::makeRect(r.x, r.y, r.width, top),
            Rect::makeRect(u0, v0, u1, top),
            &trackTexture);

        // CENTER
        targetRT.drawSprite(
            Rect::makeRect(r.x, r.y + top, r.width, centerH),
            Rect::makeRect(u0, v1, u1, v2 - v1),
            &trackTexture);

        // BOTTOM
        targetRT.drawSprite(
            Rect::makeRect(r.x, r.y + top + centerH, r.width, bottom),
            Rect::makeRect(u0, v2, u1, bottom),
            &trackTexture);
    }
    else
    {
        float left = trackStartSize;
        float right = trackEndSize;

        float centerW = r.width - left - right;

        float u0 = 0;
        float u1 = left;
        float u2 = texSize.x - right;

        float v0 = 0;
        float v1 = texSize.y;

        // LEFT
        targetRT.drawSprite(
            Rect::makeRect(r.x, r.y, left, r.height),
            Rect::makeRect(u0, v0, left, v1),
            &trackTexture);

        // CENTER
        targetRT.drawSprite(
            Rect::makeRect(r.x + left, r.y, centerW, r.height),
            Rect::makeRect(u1, v0, u2 - u1, v1),
            &trackTexture);

        // RIGHT
        targetRT.drawSprite(
            Rect::makeRect(r.x + left + centerW, r.y, right, r.height),
            Rect::makeRect(u2, v0, right, v1),
            &trackTexture);
    }
}

void ScrollBar::drawThumb(RenderTarget &targetRT, ScrollAxis a)
{
    const Rect &r = thumbRect;
    Color tint = normalTint;
    if (hover) {
        tint = hoverTint;
    }

    const Vec2u& texSize = thumbTextureSize;
    if (a == ScrollAxis::Vertical)
    {
        float top = thumbStartSize;
        float bottom = thumbEndSize;
        float centerH = r.height - top - bottom;
        float u0 = 0;
        float u1 = texSize.x;
        float v0 = 0;
        float v1 = top;
        float v2 = texSize.y - bottom;
        float v3 = texSize.y; // TOP
        targetRT.drawSprite(Rect::makeRect(r.x, r.y, r.width, top), Rect::makeRect(u0, v0, u1, top), &thumbTexture, tint);
        targetRT.drawSprite(Rect::makeRect(r.x, r.y + top, r.width, centerH), Rect::makeRect(u0, v1, u1, v2 - v1), &thumbTexture, tint);
        targetRT.drawSprite(Rect::makeRect(r.x, r.y + top + centerH, r.width, bottom), Rect::makeRect(u0, v2, u1, bottom), &thumbTexture, tint);
    }
    else
    {
        float left = thumbStartSize;
        float right = thumbEndSize;
        float centerW = r.width - left - right;
        float u0 = 0;
        float u1 = left;
        float u2 = texSize.x - right;
        float u3 = texSize.x;
        float v0 = 0;
        float v1 = texSize.y;
        targetRT.drawSprite(Rect::makeRect(r.x, r.y, left, r.height), Rect::makeRect(u0, v0, left, v1), &thumbTexture, tint);
        targetRT.drawSprite(Rect::makeRect(r.x + left, r.y, centerW, r.height), Rect::makeRect(u1, v0, u2 - u1, v1), &thumbTexture, tint);
        targetRT.drawSprite(Rect::makeRect(r.x + left + centerW, r.y, right, r.height), Rect::makeRect(u2, v0, right, v1), &thumbTexture, tint);
    }
}

float ScrollBar::getThumbSize() const
{
    if (!target)
        return 1.0f;

    ScrollAxis a = effectiveAxis();

    Vec2 contentSize = target->getContentSize();
    Vec2 viewSize = target->getViewSize();

    if (a == ScrollAxis::Vertical)
    {
        if (contentSize.y <= 0.0f)
            return 1.0f;
        return std::clamp(viewSize.y / contentSize.y, 0.05f, 1.0f);
    }
    else
    {
        if (contentSize.x <= 0.0f)
            return 1.0f;
        return std::clamp(viewSize.x / contentSize.x, 0.05f, 1.0f);
    }
}

ScrollAxis ScrollBar::effectiveAxis() const
{
    if (axis == ScrollAxis::HorizontalAndVertical)
        return ScrollAxis::Horizontal;
    return axis;
}

void ScrollBar::setNormalColor(Color c)
{
    if (normalTint != c){
        normalTint = c;
    }
}

void ScrollBar::setHoverColor(Color c)
{
    if (hoverTint != c){
        hoverTint = c;
    }
}

void ScrollBar::setTarget(ScrollContainer *t)
{
    if (t != nullptr && t != target) {
        target = t;
    }
}

void ScrollBar::setAxis(ScrollAxis axis)
{
    if (axis != this->axis) {
        this->axis = axis;
    }
}

void ScrollBar::setTrackSize(float start, float end)
{
    if (start != trackStartSize) {
        trackStartSize = start;
    }
    if (end != trackEndSize) {
        trackEndSize = end;
    }
}

void ScrollBar::setThumbSize(float start, float end)
{
    if (start != thumbStartSize) {
        thumbStartSize = start;
    }
    if (end != thumbEndSize) {
        thumbEndSize = end;
    }
}
