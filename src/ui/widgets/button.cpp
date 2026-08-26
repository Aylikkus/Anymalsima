#include "button.hpp"
#include <assert.h>

bool Button::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!enabled || !visible)
        return false;

    // hover
    if (e.type == InputEvent::MouseMove)
    {
        bool newHover = contains(e.x, e.y);

        if (newHover != hover)
        {
            hover = newHover;
            if (hover)
            {
                onHover.emit();
            }
        }

        return hover;
    }

    // нажали кнопку
    if (e.type == InputEvent::MouseClick && e.pressed)
    {
        if (contains(e.x, e.y))
        {
            pressed = true;
            onPressed.emit();
            return true;
        }
    }

    // отпустили кнопку
    if (e.type == InputEvent::MouseClick && !e.pressed)
    {
        if (pressed)
        {
            pressed = false;

            onReleased.emit();

            if (contains(e.x, e.y))
            {
                context.audioSystem->playSound(click_sound, pitch_dis(gen));
                onClick.emit();
            }

            return true;
        }
    }

    return false;
}

void Button::draw(RenderTarget &target, const UIContext &context)
{
    if (!enabled)
        return;

    if (visible)
    {
        const Rect &r = computedRect;

        float pressOffset = pressed ? 1.0f : 0.0f;

        float x = r.x + pressOffset;
        float y = r.y + pressOffset;
        float w = r.width;
        float h = r.height;

        float l = left;
        float rgt = right;
        float t = top;
        float b = bottom;

        float centerW = w - l - rgt;
        float centerH = h - t - b;

        const Vec2u& texSize = texture_size;

        // UV (в пикселях)
        float u0 = 0;
        float u1 = l;
        float u2 = texSize.x - rgt;
        float u3 = texSize.x;

        float v0 = 0;
        float v1 = t;
        float v2 = texSize.y - b;
        float v3 = texSize.y;

        Color tint = normalTint;

        if (pressed)
        {
            tint = pressedTint;
        }
        else if (hover)
        {
            tint = hoverTint;
        }

        // --- TOP ROW ---
        target.drawSprite(
            Rect::makeRect(x, y, l, t),
            Rect::makeRect(u0, v0, l, t),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l, y, centerW, t),
            Rect::makeRect(u1, v0, u2 - u1, t),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l + centerW, y, rgt, t),
            Rect::makeRect(u2, v0, rgt, t),
            &texture, tint);

        // --- MIDDLE ROW ---
        target.drawSprite(
            Rect::makeRect(x, y + t, l, centerH),
            Rect::makeRect(u0, v1, l, v2 - v1),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l, y + t, centerW, centerH),
            Rect::makeRect(u1, v1, u2 - u1, v2 - v1),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l + centerW, y + t, rgt, centerH),
            Rect::makeRect(u2, v1, rgt, v2 - v1),
            &texture, tint);

        // --- BOTTOM ROW ---
        target.drawSprite(
            Rect::makeRect(x, y + t + centerH, l, b),
            Rect::makeRect(u0, v2, l, b),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l, y + t + centerH, centerW, b),
            Rect::makeRect(u1, v2, u2 - u1, b),
            &texture, tint);

        target.drawSprite(
            Rect::makeRect(x + l + centerW, y + t + centerH, rgt, b),
            Rect::makeRect(u2, v2, rgt, b),
            &texture, tint);
    }

    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        (*it)->draw(target, context);
    }
}

Button::Button(const UIContext& context, const std::string& texture, const std::string& click_sound)
{
    this->texture = texture;
    this->click_sound = click_sound;
    this->texture_size = context.assets->getTextureSize(texture);

    std::random_device rd; 
    gen = std::mt19937(rd());
    pitch_dis = std::uniform_real_distribution<float>(0.8f, 1.2f);
}

void Button::setNormalColor(Color c)
{
    if (normalTint != c)
    {
        normalTint = c;
    }
}

void Button::setHoverColor(Color c)
{
    if (hoverTint != c)
    {
        hoverTint = c;
    }
}

void Button::setPressedColor(Color c)
{
    if (pressedTint != c)
    {
        pressedTint = c;
    }
}

void Button::setSliceBorders(float l, float t, float r, float b)
{
    if (left != l)
    {
        left = l;
    }
    if (top != t)
    {
        top = t;
    }
    if (right != r)
    {
        right = r;
    }
    if (bottom != b)
    {
        bottom = b;
    }
}


Color Button::getNormalColor()
{
    return normalTint;
}

Color Button::getHoverColor()
{
    return hoverTint;
}

Color Button::getPressedColor()
{
    return pressedTint;
}
