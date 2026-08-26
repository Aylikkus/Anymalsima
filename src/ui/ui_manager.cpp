#include "ui_manager.hpp"

#include <assert.h>

void UIManager::setRoot(std::unique_ptr<BaseWidget> r)
{
    if (root)
        removeFromMapRecursive(root.get()); // clean up old root if replacing

    WidgetID id = nextID();
    r->setID(id);
    widgetMap[id] = r.get();
    root = std::move(r);
}

void UIManager::render(RenderTarget &target, const UIContext &context)
{
    if (!root)
        return;

    root->updateLayout({0, 0, size.x, size.y}, context);
    root->draw(target, context);
}

bool UIManager::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!root)
        return false;
    
    root->resetHover();
    return root->handleInput(e, context);
}

UIManager::UIManager(UIContext &context)
{
    size = context.getWindowSize();
    context.onResize.connect([this](Vec2 newSize)
                             { size = newSize; });
}

BaseWidget *UIManager::getWidget(WidgetID id)
{
    auto it = widgetMap.find(id);
    return it != widgetMap.end() ? it->second : nullptr;
}

void UIManager::destroyWidget(WidgetID id)
{
    auto it = widgetMap.find(id);
    if (it == widgetMap.end())
        return;

    BaseWidget *widget = it->second;

    if (widget == root.get())
        return;

    BaseWidget *parent = widget->getParent();
    if (!parent)
        return;

    removeFromMapRecursive(widget);
    parent->removeChildByID(id);
}

void UIManager::removeFromMapRecursive(BaseWidget *w)
{
    WidgetID id = w->getID();
    widgetMap.erase(id);
    freeIds.push(id);

    for (auto &child : w->getChildren())
    {
        removeFromMapRecursive(child.get());
    }
}
