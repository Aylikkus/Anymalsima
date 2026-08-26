#include "text.hpp"

void Text::draw(RenderTarget &target, const UIContext &context)
{
    if (!visible)
        return;

    target.drawRect(computedRect, background);
    
    target.drawText(
        text,
        computedRect,
        fontRole,
        context.resolveFontSize(sizeRole),
        alignH,
        alignV,
        foreground);
    
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        (*it)->draw(target, context);
    }
}

void Text::setText(const std::string &value)
{
    if (text != value)
    {
        text = value;
    }
}

void Text::setFontSizeRole(FontSizeRole value)
{
    if (sizeRole != value)
    {
        sizeRole = value;
    }
}

void Text::setFontRole(FontRole value)
{
    if (fontRole != value)
    {
        fontRole = value;
    }
}

void Text::setColor(const Color &value)
{
    if (foreground != value)
    {
        foreground = value;
    }
}

void Text::setHorizontalAlign(TextAlign value)
{
    if (alignH != value)
    {
        alignH = value;
    }
}

void Text::setVerticalAlign(VerticalAlign value)
{
    if (alignV != value)
    {
        alignV = value;
    }
}

const std::string &Text::getText() const
{
    return text;
}

FontSizeRole Text::getFontSizeRole() const
{
    return sizeRole;
}

FontRole Text::getFontRole() const
{
    return fontRole;
}

const Color &Text::getColor() const
{
    return foreground;
}

TextAlign Text::getAlign() const
{
    return alignH;
}