#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include "render/render_target.hpp"
#include "core/geometry.hpp"
#include "core/input_system.hpp"
#include "core/color.hpp"
#include "render/ui_context.hpp"
#include "render/grid.hpp"
#include "core/signal.hpp"
#include "animation/animator.hpp"

using WidgetID = uint64_t;

enum class Pivot
{
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

class BaseWidget
{
public:
    virtual ~BaseWidget() = default;

    BaseWidget *getParent() const;
    const std::vector<std::unique_ptr<BaseWidget>> &getChildren() const;
    Rect getComputedRect() const;

    void setID(WidgetID v) { id = v; }
    WidgetID getID() const { return id; }
    void addChild(std::unique_ptr<BaseWidget> child);
    void setAnchor(Anchor a);
    Anchor getAnchor();
    void setOffset(Offset o);
    Offset getOffset();
    void setFixedSize(float width, float height,
                      Pivot pivot = Pivot::TopLeft,
                      float marginLeft = 0,
                      float marginTop = 0,
                      float marginRight = 0,
                      float marginBottom = 0);
    void setGridPlacement(const GridPlacement &p);
    virtual void updateLayout(const Rect &parentRect, const UIContext &context);
    virtual void draw(RenderTarget &target, const UIContext &context);
    virtual bool handleInput(const InputEvent &e, const UIContext &context);
    void setBackgroundColor(Color c);
    void setVisible(bool v);
    void setEnabled(bool v);
    Color getBackgroundColor();
    bool getVisible();
    bool getEnabled();

    bool removeChildByID(WidgetID id);

    bool contains(int x, int y);

    void setGrid(UIGrid g);
    const UIGrid *getGrid() const;

    void setGridHighlight(bool v);

    void resetHover();

protected:
    BaseWidget *parent = nullptr;
    Anchor anchor{0, 0, 0, 0};
    Offset offset{0, 0, 0, 0};
    Color background{0, 0, 0, 0};
    bool visible = true;
    bool enabled = true;
    bool hover = false;

    std::optional<UIGrid> grid;

    Rect computedRect;
    std::vector<std::unique_ptr<BaseWidget>> children;

    float uiScale = 1.0f;
    WidgetID id = 0;

private:
    Rect computeRect(const Rect &parent);
    bool highlightGrid = false;
};

using Panel = BaseWidget;

namespace Animation
{
    inline void animateGridPlacement(
        Animator &animator,
        BaseWidget *w,
        GridPlacement from,
        GridPlacement to,
        float duration)
    {
        Tween t;
        t.duration = duration;
        t.apply = [=](float t)
        {
            GridPlacement p;
            p.col = from.col + (to.col - from.col) * t;
            p.row = from.row + (to.row - from.row) * t;
            p.colSpan = from.colSpan + (to.colSpan - from.colSpan) * t;
            p.rowSpan = from.rowSpan + (to.rowSpan - from.rowSpan) * t;

            p.paddingLeft = from.paddingLeft + (to.paddingLeft - from.paddingLeft) * t;
            p.paddingTop = from.paddingTop + (to.paddingTop - from.paddingTop) * t;
            p.paddingRight = from.paddingRight + (to.paddingRight - from.paddingRight) * t;
            p.paddingBottom = from.paddingBottom + (to.paddingBottom - from.paddingBottom) * t;

            w->setGridPlacement(p);
        };
        t.easing = Ease::smooth;

        animator.addTween(std::move(t));
    }

    inline void animateAlpha(
        Animator &animator,
        BaseWidget *w,
        float from,
        float to,
        float duration)
    {
        Tween t;
        t.duration = duration;
        t.apply = [=](float t)
        {
            Color c = w->getBackgroundColor();
            c.a = (uint8_t)(from + (to - from) * t);
            w->setBackgroundColor(c);
        };
        t.easing = Ease::smooth;

        animator.addTween(std::move(t));
    }
}