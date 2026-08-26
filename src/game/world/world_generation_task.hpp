#pragma once

#include <mutex>
#include <thread>

#include "core/signal.hpp"
#include "world_generator.hpp"

class WorldGenTask {
   public:
    void start(const WorldGenSettings& settings, IAssets* assets);

    ~WorldGenTask();

    bool isFinished() const;
    float getProgress() const;

    void update();
    void cancelTask();

    Signal<> onFinished;

    std::unique_ptr<World> takeResult();

   private:
    std::thread worker;
    std::mutex mtx;

    std::atomic<bool> finished = false;
    std::atomic<float> progress = 0.0f;

    std::atomic<bool> cancel = false;

    std::unique_ptr<World> result = nullptr;
};