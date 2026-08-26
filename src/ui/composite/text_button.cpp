#include "text_button.hpp"

TextButton::TextButton(const UIContext& context, const std::string& texture, const std::string& click_sound)
{
    // Create Button
    auto btn = std::make_unique<Button>(context, texture, click_sound);
    btn->setAnchor({0, 0, 1, 1});

    // Create Text
    auto txt = std::make_unique<Text>();
    txt->setAnchor({0, 0, 1, 1});

    // Store raw pointers BEFORE move
    button = btn.get();
    text = txt.get();
    normalTextColor = text->getColor();

    // Forward signals
    button->onClick.connect([this]()
    { 
        onClick.emit(); 
    });
    button->onHover.connect([this]()
    { 
        onHover.emit(); 
    });
    button->onPressed.connect([this]()
    {
        text->setColor(button->getPressedColor());
        text->setOffset({1,1,0,0});
        onPressed.emit(); 
    });
    button->onReleased.connect([this]()
    { 
        text->setColor(normalTextColor);
        text->setOffset({0,0,0,0});
        onReleased.emit(); 
    });

    // Compose
    addChild(std::move(txt));
    addChild(std::move(btn));
}

void TextButton::setText(const std::string &v)
{
    text->setText(v);
}

void TextButton::setFontSizeRole(FontSizeRole role)
{
    text->setFontSizeRole(role);
}

void TextButton::setFontRole(FontRole role)
{
    text->setFontRole(role);
}

void TextButton::setTextColor(Color c)
{
    normalTextColor = c;
    text->setColor(c);
}

void TextButton::setTextAlignment(TextAlign alignH, VerticalAlign alignV)
{
    text->setHorizontalAlign(alignH);
    text->setVerticalAlign(alignV);
}

Button *TextButton::getButton() { return button; }
Text *TextButton::getTextWidget() { return text; }