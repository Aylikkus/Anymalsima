#pragma once

#include <string>
#include "base_widget.hpp"
#include "render/renderer_api.hpp"

class TextBox : public BaseWidget {
public:
    bool readonly = false;

    TextBox(const UIContext& context, const std::string& texture);

    void setText(const std::string& v);
    const std::string& getText() const;

    void setPlaceholder(const std::string& v);
    void setMultiline(bool v);

    void setBackgroundColor(Color c);
    void setColor(Color c);
    void setPlaceholderColor(Color c);
    void setFontSize(FontSizeRole size);
    void setFontRole(FontRole role);

    void setHorizontalAlign(TextAlign value);
    void setVerticalAlign(VerticalAlign value);

    bool handleInput(const InputEvent& e, const UIContext& context) override;
    void draw(RenderTarget& target, const UIContext& context) override;

    bool hasSelection() const;

    void clearSelection();

    void setSelection(size_t a, size_t b);

    void deleteSelection();

    virtual void updateLayout(const Rect& r, const UIContext& c) override;

private:
    const IRenderer* builder;

    std::string text;
    std::string placeholder;

    bool focused = false;
    bool multiline = false;

    float scrollX = 0.0f;
    float scrollY = 0.0f;
    float scrollSpeed = 30.0f;

    size_t caret = 0;
    size_t char_limit = 2000;

    size_t selectionAnchor = 0;
    size_t selectionStart = 0;
    size_t selectionEnd = 0;
    bool selectingWithMouse = false;

    float paddingLeft = 16, paddingRight = 16;
    float paddingTop = 16, paddingBottom = 16;

    std::string texture;
    Vec2u texture_size;
    float left = 16, right = 16, top = 16, bottom = 16;

    Color placeholderColor = {152,152,152,255};
    Color textColor = {255,255,255,255};
    Color normalTint = {232,232,232,255};

    FontSizeRole fontSize = FontSizeRole::Medium;
    FontRole fontRole = FontRole::Regular;

    TextAlign alignH = TextAlign::Left;
    VerticalAlign alignV = VerticalAlign::Top;

    TextLayoutState layoutState;
    bool layoutDirty = true;
    bool selectionDirty = true;

    Vec2 caretPos{0,0};
    bool caretDirty = true;

    float prevScale = 0.0f;

    void rebuildLayout(const UIContext& context);
    void rebuildSelection(const UIContext& context);

    void updateVerticalScroll(const Vec2& caretPos, float lineHeight, const UIContext& context);
    void updateHorizontalScroll(float caretX, const UIContext& context);
    void clampScroll(const UIContext& context);
    void moveScrollToCaret(const UIContext& context);

    size_t findWordLeft(size_t pos) const;
    size_t findWordRight(size_t pos) const;
    void applyTextErase(size_t pos, size_t len);
};