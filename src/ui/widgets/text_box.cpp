#include "text_box.hpp"
#include "core/clipboard.hpp"
#include <assert.h>

TextBox::TextBox(const UIContext& context, const std::string& texture) {
    this->texture = texture;
    this->texture_size = context.assets->getTextureSize(texture);
}

void TextBox::setText(const std::string &v)
{
    text = v;
    layoutDirty = true;
    selectionDirty = true;
    caretDirty = true;
}

const std::string &TextBox::getText() const
{
    return text;
}

void TextBox::setPlaceholder(const std::string &v)
{
    placeholder = v;
    layoutDirty = true;
}

void TextBox::setMultiline(bool v)
{
    multiline = v;
    layoutDirty = true;
}

void TextBox::setBackgroundColor(Color c)
{
    normalTint = c;
}

void TextBox::setColor(Color c)
{
    textColor = c;
}

void TextBox::setPlaceholderColor(Color c)
{
    placeholderColor = c;
}

void TextBox::setFontSize(FontSizeRole size)
{
    fontSize = size;
    layoutDirty = true;
}

void TextBox::setFontRole(FontRole role)
{
    fontRole = role;
    layoutDirty = true;
}

void TextBox::setHorizontalAlign(TextAlign value)
{
    alignH = value;
    layoutDirty = true;
}

void TextBox::setVerticalAlign(VerticalAlign value)
{
    alignV = value;
    layoutDirty = true;
}

void TextBox::rebuildLayout(const UIContext &context)
{
    TextLayoutState newState;

    float s = context.scale();
    uint32_t size = context.resolveFontSize(fontSize);

    // --- USE SCALED RECT ---
    Rect &r = computedRect;

    Rect inner = {
        r.x + paddingLeft * s,
        r.y + paddingTop * s,
        r.width - (paddingLeft + paddingRight) * s,
        r.height - (paddingTop + paddingBottom) * s};

    newState.text = text;
    newState.rect = inner;
    newState.scroll = {scrollX, scrollY};
    newState.fontRole = fontRole;
    newState.fontSize = size;
    newState.wrap = multiline;
    newState.alignH = alignH;
    newState.alignV = alignV;

    layoutState = std::move(newState);

    layoutState.markLayoutDirty();

    context.renderer->rebuild(layoutState);

    layoutDirty = false;
    selectionDirty = true;
    caretDirty = true;
}

void TextBox::rebuildSelection(const UIContext &context)
{
    if (!hasSelection())
        return;

    context.renderer->buildSelectionRects(
        layoutState,
        selectionStart,
        selectionEnd);

    selectionDirty = false;
}

void TextBox::draw(RenderTarget &target, const UIContext &context)
{
    if (!enabled || !visible)
        return;
    
    float s = context.scale();
    builder = context.renderer;

    const Rect &r = computedRect;

    float x = r.x;
    float y = r.y;
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

    layoutState.scroll = {scrollX, scrollY};

    if (layoutDirty)
    {
        rebuildLayout(context);
    }
    else if (layoutState.incrementDirty)
    {
        layoutState.text = text;
        context.renderer->rebuildIncrementally(layoutState);
        layoutState.incrementDirty = false;
    }

    Rect inner = {
        r.x + paddingLeft * s,
        r.y + paddingTop * s,
        r.width - (paddingLeft + paddingRight) * s,
        r.height - (paddingTop + paddingBottom) * s};

    target.pushClip(inner);

    // --- selection ---
    if (hasSelection())
    {
        if (selectionDirty)
            rebuildSelection(context);

        for (auto &rr : layoutState.selectionRects)
        {
            target.drawRect({rr.x,
                             rr.y,
                             rr.width,
                             rr.height},
                            {173, 216, 230, 96});
        }
    }

    // --- text ---
    Color color = text.empty() ? placeholderColor : textColor;

    target.drawTextLayout(layoutState, color);

    // --- caret ---
    if (focused && context.renderer)
    {
        caretPos = context.renderer->caretPosition(layoutState, caret);

        target.drawRect(
            {caretPos.x, caretPos.y, 1.0f, (float)layoutState.fontSize},
            {255, 255, 255, 255});
    }

    clampScroll(context);

    target.popClip();
}

void TextBox::clampScroll(const UIContext &context)
{
    if (multiline)
    {
        scrollX = 0;

        float maxScrollY = std::max(
            0.0f,
            layoutState.totalHeight - layoutState.rect.height);

        scrollY = std::clamp(scrollY, 0.0f, maxScrollY);
    }
    else
    {
        scrollY = 0;

        float maxScrollX = std::max(
            0.0f,
            layoutState.maxLineWidth - layoutState.rect.width);

        scrollX = std::clamp(scrollX, 0.0f, maxScrollX);
    }

}

bool TextBox::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!enabled || !visible)
        return false;

    float s = context.scale();
    builder = context.renderer;

    Rect inner = {
        computedRect.x + paddingLeft * s,
        computedRect.y + paddingTop * s,
        computedRect.width - (paddingLeft + paddingRight) * s,
        computedRect.height - (paddingTop + paddingBottom) * s};

    Vec2 mousePos{(float)e.x, (float)e.y};

    // ----------------------------
    // MOUSE CLICK (focus + caret)
    // ----------------------------
    if (e.type == InputEvent::MouseClick && e.pressed)
    {
        bool inside = inner.contains(mousePos.x, mousePos.y);

        if (inside)
        {
            focused = true;

            Vec2 local = {mousePos.x, mousePos.y};

            size_t pos = context.renderer->caretAtPoint(
                layoutState, local);

            caret = pos;

            if (!e.shift)
                selectionStart = pos; // anchor set here
            selectionEnd = pos;

            selectingWithMouse = true;

            selectionDirty = true;
            caretDirty = true;

            return true;
        }
        else
        {
            focused = false;
        }
    }

    // ----------------------------
    // MOUSE DRAG SELECTION
    // ----------------------------
    if (e.type == InputEvent::MouseMove && selectingWithMouse && focused)
    {
        Vec2 local = {mousePos.x, mousePos.y};

        size_t newCaret =
            context.renderer->caretAtPoint(layoutState, local);

        caret = newCaret;

        selectionEnd = newCaret;

        // IMPORTANT: anchor must be preserved, but selection must always be rebuilt
        selectionDirty = true;
        caretDirty = true;

        return true;
    }

    if (e.type == InputEvent::MouseClick && !e.pressed)
    {
        selectingWithMouse = false;
        bool inside = inner.contains(mousePos.x, mousePos.y);
        focused = inside;
        return inside;
    }

    // ----------------------------
    // MOUSE WHEEL SCROLL
    // ----------------------------
    if (e.type == InputEvent::MouseWheel)
    {
        if (!inner.contains(mousePos.x, mousePos.y)) {
            return false;
        }
        if (e.direction == InputEvent::Vertical)
        {
            if (multiline)
                scrollY -= e.delta * 30.0f;
        }
        else
        {
            scrollX -= e.delta * 30.0f;
        }

        selectionDirty = true;
        clampScroll(context);
        return true;
    }

    if (!focused)
        return false;

    // ----------------------------
    // TEXT INPUT (UTF-32 codepoint)
    // ----------------------------
    if (e.type == InputEvent::TextInput)
    {

        if (!readonly && e.codepoint != 0)
        {
            if (text.size() >= char_limit)
                return true;
            if (e.codepoint < 32)
                return true;
            if (!multiline && e.codepoint == u'\n')
                return true;
            if (hasSelection())
                deleteSelection();

            text.insert(text.begin() + caret,
                        static_cast<char16_t>(e.codepoint));
            layoutState.text = text;

            if (text.size() < 32 || e.codepoint == u'\n')
            {
                layoutDirty = true;
            }
            else
            {
                layoutState.incrementIdx = caret;
                layoutState.lastDelta = +1;
                layoutState.markIncrementDirty();
                layoutDirty = false;
            }

            caret++;
            moveScrollToCaret(context);
        }

        return true;
    }
    // ----------------------------
    // KEY INPUT
    // ----------------------------
    if (e.type == InputEvent::KeyPress && e.pressed)
    {

        switch (e.key)
        {

        case Key::SDL_SCANCODE_LEFT:
            if (caret > 0)
                caret--;
            if (e.ctrl)
            {
                caret = findWordLeft(caret);
            }
            if (e.shift)
            {
                selectionEnd = caret;
                selectionDirty = true;
            }
            else
            {
                clearSelection();
            }
            moveScrollToCaret(context);
            return true;

        case Key::SDL_SCANCODE_RIGHT:
            if (caret < text.size())
                caret++;
            if (e.ctrl)
            {
                caret = findWordRight(caret);
            }
            if (e.shift)
            {
                selectionEnd = caret;
                selectionDirty = true;
            }
            else
            {
                clearSelection();
            }
            moveScrollToCaret(context);
            return true;

        case Key::SDL_SCANCODE_UP:
        {
            Vec2 p = context.renderer->caretPosition(layoutState, caret);
            p.y -= layoutState.fontSize * 0.5f;

            caret = context.renderer->caretAtPoint(layoutState, p);
            if (e.shift)
            {
                selectionEnd = caret;
                selectionDirty = true;
            }
            else
            {
                clearSelection();
            }
            moveScrollToCaret(context);
            return true;
        }

        case Key::SDL_SCANCODE_DOWN:
        {
            Vec2 p = context.renderer->caretPosition(layoutState, caret);
            p.y += layoutState.fontSize * 2.0f;

            caret = context.renderer->caretAtPoint(layoutState, p);
            if (e.shift)
            {
                selectionEnd = caret;
                selectionDirty = true;
            }
            moveScrollToCaret(context);
            return true;
        }

        case Key::SDL_SCANCODE_BACKSPACE:
        {
            if (!readonly && caret > 0)
            {
                if (hasSelection())
                {
                    deleteSelection();
                }
                else
                {
                    if (text[caret - 1] == u'\n')
                    {
                        layoutState.removedNewLine = true;
                    }
                    applyTextErase(caret - 1, 1);
                    if (caret != text.size())
                        caret--;
                }
            }
            moveScrollToCaret(context);
            return true;
        }

        case Key::SDL_SCANCODE_DELETE:
            if (!readonly)
            {
                if (hasSelection())
                {
                    deleteSelection();
                }
                else if (caret < text.size())
                {
                    if (text[caret] == u'\n')
                    {
                        layoutState.removedNewLine = true;
                    }
                    applyTextErase(caret, 1);
                }
            }
            moveScrollToCaret(context);
            return true;

        case Key::SDL_SCANCODE_KP_ENTER:
            if (!readonly && multiline)
            {

                if (hasSelection())
                    deleteSelection();
                if (text.size() >= char_limit)
                    return true;
                text.insert(text.begin() + caret, u'\n');

                layoutState.text = text;
                layoutDirty = true;

                caret++;
            }
            moveScrollToCaret(context);
            return true;

        case Key::SDL_SCANCODE_HOME:
            caret = 0;
            selectionEnd = caret;
            selectionDirty = true;
            moveScrollToCaret(context);
            return true;

        case Key::SDL_SCANCODE_END:
            caret = text.size();
            selectionEnd = caret;
            selectionDirty = true;
            moveScrollToCaret(context);
            return true;

        default:
            break;
        }

        // ----------------------------
        // CTRL SHORTCUTS
        // ----------------------------
        if (e.ctrl)
        {

            if (e.key == Key::SDL_SCANCODE_A)
            {
                selectionStart = 0;
                selectionEnd = text.size();
                caret = selectionEnd;
                moveScrollToCaret(context);
                return true;
            }

            if (e.key == Key::SDL_SCANCODE_C)
            {
                if (hasSelection())
                {
                    int a = std::min(selectionStart, selectionEnd);
                    int b = std::max(selectionStart, selectionEnd);
                    Clipboard::setString(text.substr(a, b));
                    clearSelection();
                }
                return true;
            }

            if (e.key == Key::SDL_SCANCODE_X)
            {
                if (hasSelection())
                {
                    int a = std::min(selectionStart, selectionEnd);
                    int b = std::max(selectionStart, selectionEnd);
                    Clipboard::setString(text.substr(a, b));
                    deleteSelection();
                    caret = a;
                }
                return true;
            }

            if (e.key == Key::SDL_SCANCODE_V && !readonly)
            {
                size_t selected = hasSelection()
                    ? std::abs((int)selectionStart - (int)selectionEnd)
                    : 0;

                size_t available = char_limit - (text.size() - selected);

                if (available == 0)
                    return true;

                std::string clip = Clipboard::getString();

                if (clip.size() > available)
                    clip.resize(available);
                if (hasSelection())
                    deleteSelection();

                text.insert(caret, clip);

                layoutState.text = text;

                caret += clip.size();
                layoutDirty = true;
                return true;
            }
        }
    }

    return false;
}

size_t TextBox::findWordLeft(size_t pos) const
{
    if (text.empty() || pos == 0)
        return 0;

    pos = std::min(pos, text.size());

    while (pos > 0 && text[pos - 1] != u' ')
        --pos;

    while (pos > 0 && text[pos - 1] != u' ')
        --pos;

    return pos;
}

size_t TextBox::findWordRight(size_t pos) const
{
    if (text.empty())
        return 0;

    pos = std::min(pos, text.size());

    while (pos < text.size() && text[pos] != u' ')
        ++pos;

    while (pos < text.size() && text[pos] != u' ')
        ++pos;

    return pos;
}

void TextBox::updateLayout(const Rect &r, const UIContext &c)
{
    BaseWidget::updateLayout(r, c);
    if (c.scale() != prevScale)
    {
        rebuildLayout(c);
    }

    builder = c.renderer;

    prevScale = c.scale();
}

void TextBox::applyTextErase(size_t pos, size_t len)
{
    if (pos > text.size())
        return;

    text.erase(text.begin() + pos, text.begin() + pos + len);
    layoutState.text = text;
    if (len == 1) {
        layoutState.incrementIdx = pos;
        layoutState.lastDelta = -1;
        layoutState.markIncrementDirty();
    } else {
        layoutDirty = true;
    }

    caret = std::min(caret, text.size());
    selectionStart = selectionEnd = caret;

    selectionDirty = true;
    caretDirty = true;
}

void TextBox::moveScrollToCaret(const UIContext &context)
{
    Vec2 p = context.renderer->caretPosition(layoutState, caret, true);

    float viewW = layoutState.rect.width;
    float viewH = layoutState.rect.height;

    float caretTop = p.y - layoutState.rect.y;
    float caretBottom = caretTop + layoutState.lineHeight;

    float caretLeft = p.x - layoutState.rect.x;
    float caretRight = caretLeft;

    float newScrollX = scrollX;
    float newScrollY = scrollY;

    if (multiline)
    {
        float visibleTop = scrollY;
        float visibleBottom = scrollY + viewH;

        if (caretTop < visibleTop)
        {
            newScrollY = caretTop;
        }
        else if (caretBottom > visibleBottom)
        {
            newScrollY = caretBottom - viewH;
        }
    }
    else
    {
        float visibleLeft = scrollX;
        float visibleRight = scrollX + viewW;

        if (caretLeft < visibleLeft)
        {
            newScrollX = caretLeft;
        }
        else if (caretRight > visibleRight)
        {
            newScrollX = caretRight - viewW;
        }
    }

    scrollX = newScrollX;
    scrollY = newScrollY;

    clampScroll(context);
}

bool TextBox::hasSelection() const
{
    return selectionStart != selectionEnd;
}

void TextBox::clearSelection()
{
    caret = std::min(caret, text.size());
    selectionStart = selectionEnd = caret;
    selectionDirty = true;

    layoutState.selectionRects.clear();
}

void TextBox::setSelection(size_t a, size_t b)
{
    size_t n = text.size();
    a = std::min(a, n);
    b = std::min(b, n);

    selectionStart = std::min(a, b);
    selectionEnd = std::max(a, b);
    caret = selectionEnd;
    selectionDirty = true;
}

void TextBox::deleteSelection()
{
    if (!hasSelection())
        return;

    if (selectionStart > selectionEnd)
        std::swap(selectionStart, selectionEnd);

    applyTextErase(selectionStart, selectionEnd - selectionStart);

    caret = selectionStart;
    selectionStart = selectionEnd = caret;

    clearSelection();
}
