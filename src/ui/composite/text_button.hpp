#pragma once

#include "widgets/button.hpp"
#include "widgets/text.hpp"

class TextButton : public BaseWidget {
public:
    TextButton(const UIContext& context, const std::string& texture, const std::string& click_sound);

    // API
    void setText(const std::string& text);
    void setFontSizeRole(FontSizeRole role);
    void setFontRole(FontRole role);
    void setTextColor(Color c);
    void setTextAlignment(TextAlign alignH, VerticalAlign alignV);

    Button* getButton();
    Text* getTextWidget();

    // Signals (forwarded)
    Signal<> onClick;
    Signal<> onHover;
    Signal<> onPressed;
    Signal<> onReleased;

private:
    Button* button = nullptr;
    Text* text = nullptr;
    Color normalTextColor;
};