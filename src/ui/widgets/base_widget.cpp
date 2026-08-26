#include "base_widget.hpp"
#include <iostream>
#include <assert.h>

BaseWidget *BaseWidget::getParent() const
{
    return parent;
}

const std::vector<std::unique_ptr<BaseWidget>> &BaseWidget::getChildren() const
{
    return children;
}

Rect BaseWidget::getComputedRect() const
{
    return computedRect;
}

void BaseWidget::addChild(std::unique_ptr<BaseWidget> child)
{
    child->parent = this;
    children.push_back(std::move(child));
}

void BaseWidget::updateLayout(const Rect &parentRect, const UIContext &context)
{
    computedRect = computeRect(parentRect);

    for (auto &child : children)
    {
        child->updateLayout(computedRect, context);
    }
}

void BaseWidget::draw(RenderTarget &target, const UIContext &context)
{
    if (!enabled || !visible)
        return;

    target.drawRect(computedRect, background);

    if (highlightGrid && grid)
    {
        const float cw = computedRect.width / grid->columns;
        const float ch = computedRect.height / grid->rows;

        for (int col = 0; col <= grid->columns; ++col)
        {
            float x = computedRect.x + col * cw;
            target.drawRect({x, computedRect.y, 1.0f, computedRect.height}, {255,255,255,80});
        }

        for (int row = 0; row <= grid->rows; ++row)
        {
            float y = computedRect.y + row * ch;
            target.drawRect({computedRect.x, y, computedRect.width, 1.0f}, {255,255,255,80});
        }
    }

    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        (*it)->draw(target, context);
    }
}

bool BaseWidget::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!enabled || !visible)
        return false;

    for (auto it = children.begin(); it != children.end(); ++it)
    {
        if ((*it)->handleInput(e, context))
            return true;
    }
    return false;
}

bool BaseWidget::contains(int x, int y)
{
    return x >= computedRect.x && x <= computedRect.x + computedRect.width && y >= computedRect.y && y <= computedRect.y + computedRect.height;
}

Rect BaseWidget::computeRect(const Rect &parent)
{
    float x = parent.x + anchor.minX * parent.width + offset.left;
    float y = parent.y + anchor.minY * parent.height + offset.top;

    float right = parent.x + anchor.maxX * parent.width - offset.right;
    float bottom = parent.y + anchor.maxY * parent.height - offset.bottom;

    return {x, y, right - x, bottom - y};
}

void BaseWidget::setAnchor(Anchor a)
{
    anchor = a;
}

Anchor BaseWidget::getAnchor()
{
    return anchor;
}

void BaseWidget::setOffset(Offset o)
{
    offset = o;
}

void BaseWidget::setFixedSize(float width, float height,
                              Pivot pivot,
                              float marginLeft,
                              float marginTop,
                              float marginRight,
                              float marginBottom)
{
    float minX, minY, maxX, maxY;
    float offLeft, offTop, offRight, offBottom;

    // Вычисляем anchor по pivot
    switch (pivot)
    {
    case Pivot::TopLeft:
        minX = 0;
        minY = 0;
        break;
    case Pivot::TopCenter:
        minX = 0.5f;
        minY = 0;
        break;
    case Pivot::TopRight:
        minX = 1;
        minY = 0;
        break;
    case Pivot::CenterLeft:
        minX = 0;
        minY = 0.5f;
        break;
    case Pivot::Center:
        minX = 0.5f;
        minY = 0.5f;
        break;
    case Pivot::CenterRight:
        minX = 1;
        minY = 0.5f;
        break;
    case Pivot::BottomLeft:
        minX = 0;
        minY = 1;
        break;
    case Pivot::BottomCenter:
        minX = 0.5f;
        minY = 1;
        break;
    case Pivot::BottomRight:
        minX = 1;
        minY = 1;
        break;
    }
    maxX = minX;
    maxY = minY;

    // Вычисляем offset для фиксированного размера
    // При pivot в углу/краю: отступы от края + размер внутрь
    // При pivot в центре: центрирование с отступами

    if (minX == 0)
    { // левый край
        offLeft = marginLeft;
        offRight = -(marginLeft + width);
    }
    else if (minX == 0.5f)
    { // центр по X
        offLeft = -(width / 2 + marginLeft - marginRight);
        offRight = -(width / 2 + marginRight - marginLeft);
    }
    else
    { // правый край
        offLeft = -(marginRight + width);
        offRight = marginRight;
    }

    if (minY == 0)
    { // верхний край
        offTop = marginTop;
        offBottom = -(marginTop + height);
    }
    else if (minY == 0.5f)
    { // центр по Y
        offTop = -(height / 2 + marginTop - marginBottom);
        offBottom = -(height / 2 + marginBottom - marginTop);
    }
    else
    { // нижний край
        offTop = -(marginBottom + height);
        offBottom = marginBottom;
    }

    setAnchor({minX, minY, maxX, maxY});
    setOffset({offLeft, offTop, offRight, offBottom});
}

void BaseWidget::setGrid(UIGrid g)
{
    grid = g;
}

const UIGrid *BaseWidget::getGrid() const
{
    return grid ? &(*grid) : nullptr;
}

void BaseWidget::setGridHighlight(bool v)
{
    if (v != highlightGrid)
    {
        highlightGrid = v;
    }
}

void BaseWidget::setGridPlacement(const GridPlacement &p)
{
    assert(parent && parent->grid && "Parent must have Grid set");

    const UIGrid &grid = *parent->grid;

    const float cw = grid.cellWidth();
    const float ch = grid.cellHeight();

    float minX = p.col * cw;
    float minY = p.row * ch;
    float maxX = (p.col + p.colSpan) * cw;
    float maxY = (p.row + p.rowSpan) * ch;

    setAnchor({minX, minY, maxX, maxY});
    setOffset({p.paddingLeft, p.paddingTop, p.paddingRight, p.paddingBottom});
}

void BaseWidget::setBackgroundColor(Color c)
{
    if (c == background) return;
    background = c;
}

void BaseWidget::setVisible(bool v)
{
    if (v == visible) return;
    visible = v;
}

void BaseWidget::setEnabled(bool v)
{
    if (v == enabled) return;
    enabled = v;
}

bool BaseWidget::getVisible()
{
    return visible;
}

bool BaseWidget::getEnabled()
{
    return enabled;
}

Color BaseWidget::getBackgroundColor()
{
    return background;
}

bool BaseWidget::removeChildByID(WidgetID id)
{
    auto it = std::remove_if(children.begin(), children.end(),
        [id](const std::unique_ptr<BaseWidget>& c){
            return c->getID() == id;
        });

    if (it != children.end()) {
        children.erase(it, children.end());
        return true;
    }

    // recursive search
    for (auto& child : children) {
        if (child->removeChildByID(id))
            return true;
    }

    return false;
}

Offset BaseWidget::getOffset()
{
    return offset;
}

void BaseWidget::resetHover()
{
    hover = false;
    for (auto &c : children)
        c->resetHover();
}
