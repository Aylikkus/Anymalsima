#pragma once

#include "base_widget.hpp"

class Text : public BaseWidget
{
public:
    void setText(const std::string &value);
    void setFontSizeRole(FontSizeRole value);
    void setFontRole(FontRole value);
    void setColor(const Color &value);
    void setHorizontalAlign(TextAlign value);
    void setVerticalAlign(VerticalAlign value);

    const std::string &getText() const;
    FontSizeRole getFontSizeRole() const;
    FontRole getFontRole() const;
    const Color &getColor() const;
    TextAlign getAlign() const;

    virtual void draw(RenderTarget &target, const UIContext &context) override;

private:
    std::string text;
    FontSizeRole sizeRole = FontSizeRole::Medium;
    FontRole fontRole = FontRole::Regular;
    Color foreground = {255, 255, 255, 255};
    TextAlign alignH = TextAlign::Left;
    VerticalAlign alignV = VerticalAlign::Top;
};