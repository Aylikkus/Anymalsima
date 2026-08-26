#include "animator.hpp"

void Animator::update(float dt)
{
    for (auto &t : tweens)
        t.update(dt);

    tweens.erase(
        std::remove_if(tweens.begin(), tweens.end(),
                       [](const Tween &t)
                       { return t.finished; }),
        tweens.end());
}

Tween &Animator::addTween(Tween t)
{
    tweens.push_back(std::move(t));
    return tweens.back();
}

void Animator::clear()
{ 
    tweens.clear(); 
}
