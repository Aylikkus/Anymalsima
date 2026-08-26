#pragma once
#include <queue>
#include "widgets/base_widget.hpp"
#include "render/render_target.hpp"
#include "render/ui_context.hpp"
#include <unordered_map>

class UIManager {
public:
    UIManager(UIContext& context);

    void render(RenderTarget& target, const UIContext& context);

    bool handleInput(const InputEvent& e, const UIContext& context);

    WidgetID nextID() { 
        WidgetID id;
        if (!freeIds.empty()) {
            id = freeIds.front();
            freeIds.pop();
        } else {
            id = idCounter++;
        }
        return id;
    }

    void setRoot(std::unique_ptr<BaseWidget> r);

    BaseWidget* getWidget(WidgetID id);

    template <class T>
    T* createWidget(std::unique_ptr<T> w, BaseWidget* parent)
    {
        WidgetID id = nextID();
        T* ptr = w.get();

        w->setID(id);
        widgetMap[id] = ptr;

        parent->addChild(std::move(w));

        return ptr;
    }

    void destroyWidget(WidgetID id);

private:
    std::unique_ptr<BaseWidget> root;
    RenderTarget cachedCommands;
    std::unordered_map<WidgetID, BaseWidget*> widgetMap;
    Vec2 size;

    WidgetID idCounter = 0;
    std::queue<WidgetID> freeIds;

    void removeFromMapRecursive(BaseWidget* w);
};