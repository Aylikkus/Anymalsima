#pragma once

#include "text_button.hpp"
#include "widgets/scroll_container.hpp"

template <typename T>
class Dropdown : public BaseWidget
{
public:
    Dropdown(const UIContext& context, const std::string& header, const std::string& item, const std::string& click_sound);

    struct Item
    {
        std::string label;
        T value;
    };

    // data
    void addItem(const UIContext& context, const std::string &label, const T &value);

    // selection
    void setSelectedIndex(int index);
    int getSelectedIndex() const;
    const T &getValue() const;

    void setHeaderFontSize(FontSizeRole fontSize);
    void setFontStyle(FontRole fontStyle);

    // state
    void setOpen(bool v);
    bool isOpen() const;

    // signals
    Signal<const T &> onChanged;

    // overrides
    void draw(RenderTarget &target, const UIContext &context) override;
    bool handleInput(const InputEvent &e, const UIContext &context) override;
    void updateLayout(const Rect &parentRect, const UIContext &context) override;

private:
    std::vector<Item> items;
    int selectedIndex = -1;
    bool open = false;

    std::string headerTexture;
    std::string itemTexture;
    std::string clickSoundKey;

    TextButton *header = nullptr;
    ScrollContainer *list = nullptr;

    void rebuildHeader();
};

template <typename T>
Dropdown<T>::Dropdown(const UIContext& context, const std::string& headerTexture, const std::string& itemTexture, const std::string& click_sound)
{
    this->headerTexture = headerTexture;
    this->itemTexture = itemTexture;
    this->clickSoundKey = click_sound;

    // HEADER
    auto btn = std::make_unique<TextButton>(context, headerTexture, click_sound);
    btn->setAnchor({0, 0, 1, 1}); // full
    btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    header = btn.get();

    btn->onClick.connect([this]() {
        setOpen(!open);
    });

    addChild(std::move(btn));

    // LIST
    auto scroll = std::make_unique<ScrollContainer>();
    scroll->setAxis(ScrollAxis::Vertical);

    list = scroll.get();
    list->setAnchor({0, 1, 1, 3});
    list->setVisible(false);

    addChild(std::move(scroll));
}

template <typename T>
void Dropdown<T>::addItem(const UIContext& context, const std::string &label, const T &value)
{
    int index = (int)items.size();
    items.push_back({label, value});

    auto item = std::make_unique<TextButton>(context, itemTexture, clickSoundKey);
    item->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    item->setText(label);

    const float itemSize = 0.25f;

    item->setAnchor({0, itemSize * index, 1, itemSize * index + itemSize});

    item->onClick.connect([this, index]() {
        setSelectedIndex(index);
        setOpen(false);
    });

    list->addChild(std::move(item));

    if (selectedIndex == -1) {
        selectedIndex = 0;
        rebuildHeader();
    }
}

template <typename T>
void Dropdown<T>::setSelectedIndex(int index)
{
    if (index < 0 || index >= (int)items.size())
        return;

    if (selectedIndex == index)
        return;

    selectedIndex = index;
    rebuildHeader();

    onChanged.emit(items[selectedIndex].value);
}

template <typename T>
int Dropdown<T>::getSelectedIndex() const
{
    return selectedIndex;
}

template <typename T>
const T &Dropdown<T>::getValue() const
{
    assert(selectedIndex >= 0 && selectedIndex < (int)items.size());
    return items[selectedIndex].value;
}

template <typename T>
inline void Dropdown<T>::setHeaderFontSize(FontSizeRole fontSize)
{
    header->setFontSizeRole(fontSize);
}

template <typename T>
inline void Dropdown<T>::setFontStyle(FontRole fontStyle)
{
    header->setFontRole(fontStyle);
}

template <typename T>
void Dropdown<T>::setOpen(bool v)
{
    if (open == v) return;

    open = v;
    list->setVisible(open);
}

template <typename T>
bool Dropdown<T>::isOpen() const
{
    return open;
}

template <typename T>
void Dropdown<T>::draw(RenderTarget &target, const UIContext &context)
{
    if (!visible)
        return;

    BaseWidget::draw(target, context);
}

template <typename T>
bool Dropdown<T>::handleInput(const InputEvent &e, const UIContext &context)
{
    if (!enabled || !visible)
        return false;

    // children first
    if (BaseWidget::handleInput(e, context))
        return true;

    // close on outside click
    if (e.type == InputEvent::MouseClick && e.pressed) {
        Rect totalRect = computedRect;
        totalRect.height += list->getComputedRect().height;

        if (open && !totalRect.contains(e.x, e.y)) {
            setOpen(false);
        }
    }

    return false;
}

template <typename T>
void Dropdown<T>::updateLayout(const Rect &parentRect, const UIContext &context)
{

    BaseWidget::updateLayout(parentRect, context);
}

template <typename T>
void Dropdown<T>::rebuildHeader()
{
    if (selectedIndex >= 0 && selectedIndex < (int)items.size()) {
        header->setText(items[selectedIndex].label);
    } 
    else {
        header->setText("");
    }
}