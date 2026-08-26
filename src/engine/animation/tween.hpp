#pragma once

#include <functional>
#include <algorithm>

struct Tween {
    float duration = 1.0f;
    float elapsed = 0.0f;

    std::function<void(float)> apply = nullptr; // t in [0..1]
    std::function<void()> onFinish = nullptr;

    std::function<float(float)> easing = nullptr; // optional

    bool finished = false;

    void update(float dt);
};