#include "input_system.hpp"

InputSystem::InputSystem()
{
    events.reserve(128);
}

void InputSystem::add(const InputEvent &event)
{
    events.push_back(event);
}

void InputSystem::clear()
{
    events.clear();
}

const std::vector<InputEvent> &InputSystem::getEvents() const
{
    return events;
}
