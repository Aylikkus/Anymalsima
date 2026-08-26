#include "tween.hpp"

void Tween::update(float dt)
{
    if (finished)
        return;

    elapsed += dt;
    float t = std::clamp(elapsed / duration, 0.0f, 1.0f);

    if (easing)
        t = easing(t);

    if (!apply) {
        finished = true;
        return;
    }

    apply(t);

    if (elapsed >= duration)
    {
        finished = true;
        if (onFinish)
            onFinish();
    }
}