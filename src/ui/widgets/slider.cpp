#include "slider.hpp"
#include <assert.h>

Slider::Slider(const UIContext& context, const std::string& trackTexture, const std::string& thumbTexture)
{
    this->trackTexture = trackTexture;
    this->thumbTexture = thumbTexture;
    trackTextureSize = context.assets->getTextureSize(trackTexture);
    thumbTextureSize = context.assets->getTextureSize(thumbTexture);
}

void Slider::setRange(float min, float max)
{
    minValue = min;
    maxValue = max;
    setValue(value); // clamp + update
}

void Slider::setStep(float s)
{
    step = std::max(0.0f, s);
}

void Slider::setValue(float v)
{
    float clamped = std::clamp(v, minValue, maxValue);

    if (step > 0.0f)
    {
        float steps = std::round((clamped - minValue) / step);
        clamped = std::clamp(minValue + steps * step, minValue, maxValue);
    }

    if (clamped == value)
        return;

    value = clamped;
    onValueChanged.emit(value);
}

float Slider::getValue() const
{
    return value;
}

void Slider::updateLayout(const Rect &parentRect, const UIContext &context)
{
    BaseWidget::updateLayout(parentRect, context);

    float t = getNormalized();
    float s = context.scale();

    ScrollAxis a = effectiveAxis();
    float size = std::min(computedRect.width, computedRect.height);

    if (a == ScrollAxis::Vertical)
    {
        float track = computedRect.height;
        float y = computedRect.y + t * (track - size);

        thumbRect = Rect::makeRect(
            computedRect.x + (computedRect.width - size) * 0.5f, // center horizontally
            y,
            size,
            size);
    }
    else
    {
        float track = computedRect.width;
        float x = computedRect.x + t * (track - size);

        thumbRect = Rect::makeRect(
            x,
            computedRect.y + (computedRect.height - size) * 0.5f, // center vertically
            size,
            size);
    }
}

bool Slider::handleInput(const InputEvent &e, const UIContext &)
{
    if (!enabled || !visible)
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
        if (computedRect.contains(e.x, e.y))
        {
            float t;

            if (a == ScrollAxis::Vertical)
                t = (e.y - computedRect.y) / computedRect.height;
            else
                t = (e.x - computedRect.x) / computedRect.width;

            t = std::clamp(t, 0.0f, 1.0f);

            setValue(minValue + t * (maxValue - minValue));
            return true;
        }
    }

    if (e.type == InputEvent::MouseMove)
    {
        bool newHover = thumbRect.contains(e.x, e.y);
        if (newHover != hover)
        {
            hover = newHover;
        }
    }

    if (e.type == InputEvent::MouseMove && dragging)
    {
        float t;

        if (a == ScrollAxis::Vertical)
        {
            float track = computedRect.height;
            float thumbH = thumbRect.height;

            float y = e.y - dragOffset;
            t = (y - computedRect.y) / (track - thumbH);
        }
        else
        {
            float track = computedRect.width;
            float thumbW = thumbRect.width;

            float x = e.x - dragOffset;
            t = (x - computedRect.x) / (track - thumbW);
        }

        t = std::clamp(t, 0.0f, 1.0f);

        float newValue = minValue + t * (maxValue - minValue);
        setValue(newValue);

        return true;
    }

    if (e.type == InputEvent::MouseClick && !e.pressed)
    {
        dragging = false;
    }

    return false;
}

void Slider::draw(RenderTarget &targetRT, const UIContext &context)
{
    if (!enabled || !visible)
        return;

    ScrollAxis a = effectiveAxis();

    // 1. TRACK
    drawTrack(targetRT, a);

    // 2. THUMB
    drawThumb(targetRT, a);
}

void Slider::drawTrack(RenderTarget &targetRT, ScrollAxis a)
{
    const Rect &r = computedRect;

    const Vec2u &texSize = trackTextureSize;

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

void Slider::drawThumb(RenderTarget &targetRT, ScrollAxis a)
{
    const Rect &r = thumbRect;
    Color tint = normalTint;
    if (hover)
    {
        tint = hoverTint;
    }
    const Vec2u& texSize = thumbTextureSize;
    targetRT.drawSprite(
        r,
        Rect::makeRect(0, 0,
                       texSize.x,
                       texSize.y),
        &thumbTexture,
        tint);
}

float Slider::getNormalized() const
{
    if (maxValue <= minValue)
        return 0.0f;
    return (value - minValue) / (maxValue - minValue);
}

ScrollAxis Slider::effectiveAxis() const
{
    if (axis == ScrollAxis::HorizontalAndVertical)
        return ScrollAxis::Horizontal;
    return axis;
}

void Slider::setNormalColor(Color c)
{
    if (normalTint != c)
    {
        normalTint = c;
    }
}

void Slider::setHoverColor(Color c)
{
    if (hoverTint != c)
    {
        hoverTint = c;
    }
}

void Slider::setAxis(ScrollAxis axis)
{
    if (axis != this->axis)
    {
        this->axis = axis;
    }
}

void Slider::setTrackSize(float start, float end)
{
    if (start != trackStartSize)
    {
        trackStartSize = start;
    }
    if (end != trackEndSize)
    {
        trackEndSize = end;
    }
}

void Slider::setThumbSize(float start, float end)
{
    if (start != thumbStartSize)
    {
        thumbStartSize = start;
    }
    if (end != thumbEndSize)
    {
        thumbEndSize = end;
    }
}

void Slider::setThumbLength(float px)
{
    if (px == thumbLength)
        return;
    thumbLength = px;
}
