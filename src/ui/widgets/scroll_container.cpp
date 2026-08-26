#include "scroll_container.hpp"
#include <iostream>

void ScrollContainer::updateLayout(const Rect &parentRect, const UIContext &context)
{
    BaseWidget::updateLayout(parentRect, context);

    viewHeight = computedRect.height;
    viewWidth = computedRect.width;

    // считаем общую высоту контента
    contentHeight = 0.0f;
    contentWidth = 0.0f;
    for (auto &child : children)
    {
        child->updateLayout(computedRect, context);
        contentHeight = std::max(contentHeight,
                                 child->getComputedRect().y + child->getComputedRect().height - computedRect.y);
        contentWidth = std::max(contentWidth,
                                child->getComputedRect().x + child->getComputedRect().width - computedRect.x);
    }

    // clamp scroll
    float maxScrollY = std::max(0.0f, contentHeight - viewHeight);
    scrollY = std::clamp(scrollY, 0.0f, maxScrollY);

    float maxScrollX = std::max(0.0f, contentWidth - viewWidth);
    scrollX = std::clamp(scrollX, 0.0f, maxScrollX);

    // создаём смещённый rect
    contentRect = computedRect;
    contentRect.y -= scrollY;
    contentRect.x -= scrollX;

    // обновляем детей относительно contentRect
    for (auto &child : children)
    {
        child->updateLayout(contentRect, context);
    }
}

void ScrollContainer::draw(RenderTarget &target, const UIContext &context)
{
    if (!enabled)
        return;

    if (visible)
    {
        target.drawRect(computedRect, background);

        // включаем clipping
        target.pushClip(computedRect);

        for (auto it = children.rbegin(); it != children.rend(); ++it)
        {
            if (((*it)->getComputedRect().y < computedRect.y + computedRect.height) &&
                ((*it)->getComputedRect().y + (*it)->getComputedRect().height >= computedRect.y))
            {
                (*it)->draw(target, context);
            }
        }

        target.popClip();
    }
}

bool ScrollContainer::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!enabled || !visible)
        return false;

    if (e.type == InputEvent::MouseClick && !contains(e.x, e.y)) {
        return false;
    }
    // пробрасываем в детей
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        if ((*it)->handleInput(e, context))
            return true;
    }

    if (e.type == InputEvent::MouseWheel && contains(e.x, e.y))
    {
        if (axis == ScrollAxis::Vertical)
        {
            scrollY -= e.delta * scrollSpeed;
        }
        else if (axis == ScrollAxis::Horizontal)
        {
            scrollX -= e.delta * scrollSpeed;
        }
        else
        {
            if (e.direction == InputEvent::ScrollDirection::Horizontal)
            {
                scrollX -= e.delta * scrollSpeed;
            }
            else
            {
                scrollY -= e.delta * scrollSpeed;
            }
        }

        float maxScrollY = std::max(0.0f, contentHeight - viewHeight);
        scrollY = std::clamp(scrollY, 0.0f, maxScrollY);

        float maxScrollX = std::max(0.0f, contentWidth - viewWidth);
        scrollX = std::clamp(scrollX, 0.0f, maxScrollX);

        return true;
    }

    return false;
}

void ScrollContainer::setAxis(ScrollAxis axis)
{
    if (axis != this->axis)
    {
        this->axis = axis;
    }
}

void ScrollContainer::setScrollSpeed(float speed)
{
    if (speed != scrollSpeed)
    {
        scrollSpeed = speed;
    }
}

void ScrollContainer::setScroll(float x, float y)
{
    if (x != scrollX)
    {
        scrollX = x;
    }
    if (y != scrollY)
    {
        scrollY = y;
    }
}

Vec2 ScrollContainer::getScroll()
{
    return {scrollX, scrollY};
}

Vec2 ScrollContainer::getContentSize()
{
    return {contentWidth, contentHeight};
}

Vec2 ScrollContainer::getViewSize()
{
    return {viewWidth, viewHeight};
}
