#pragma once

#include "tween.hpp"

namespace Ease {
    inline float smooth(float t) {
        return t * t * (3 - 2 * t); // smoothstep
    }
}

class Animator {
public:
    void update(float dt);
    void clear();

    Tween& addTween(Tween t);

private:
    std::vector<Tween> tweens;
};

namespace Animation {
    template<typename T>
    inline void animate(
        Animator& animator,
        T from,
        T to,
        float duration,
        std::function<void(const T&)> setter
    )
    {
        Tween t;
        t.duration = duration;
        t.apply = [=](float t){ setter(from + (to - from) * t); };

        animator.addTween(std::move(t));
    }
    
    inline void delay(Animator& animator, float time, std::function<void()> cb)
    {
        Tween t;
        t.duration = time;

        t.apply = [](float) {};

        t.onFinish = std::move(cb);

        animator.addTween(std::move(t));
    }
}