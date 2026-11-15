# UI Guidelines

See the [repository guidelines](../../AGENTS.md) for shared conventions.

## Responsibilities & Structure

`ui_manager.hpp/.cpp` owns the root widget, widget-ID lookup, input dispatch, and cached render commands. `widgets/` contains the base widget, buttons, text, text boxes, sliders, scrolling controls, and world preview. `composite/` contains text buttons and dropdowns. Shared context and drawing contracts live in `../engine/render/`; menus that assemble widgets live in `../game/states/`.

## Making Changes

Build controls through the existing widget tree and `UIManager::createWidget` ownership pattern. Parents own children through unique pointers; registry pointers and callback captures must remain valid only while widgets exist. Use manager destruction paths so removed descendants leave the lookup map.

Preserve layout, anchoring, clipping, input consumption, and cached-command invalidation when changing widgets. Request rendering, assets, platform services, and audio through the existing context interfaces. Keep reusable interactions here and game-specific actions in state callbacks. Translate visible labels using the localization wrapper.

## Build & Validation

From the repository root, run `cmake --build build --target app --parallel`. Exercise changed controls with pointer and keyboard input, resize the window, change UI scale, and switch languages. For scrolling or text editing, check clipping, focus, selection, caret positioning, Unicode input, and interaction after widgets are removed or recreated.
