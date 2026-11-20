#include "world_generation_task.hpp"

void WorldGenTask::start(const WorldGenSettings& settings, IAssets* assets)
{
    if (worker.joinable())
    {
        cancel = true;
        worker.join();
    }

    cancel = false;
    finished = false;

    {
        std::lock_guard lock(mtx);
        result.reset();
    }

    worker = std::thread([this, settings, assets]() {
        WorldGenerator gen;

        auto localResult = gen.generate(settings, assets, [this]() {
            return cancel.load(std::memory_order_relaxed);
        });

        if (cancel.load(std::memory_order_relaxed))
            return;

        {
            std::lock_guard lock(mtx);
            result = std::make_unique<World>(std::move(localResult));
        }

        finished.store(true, std::memory_order_release);
    });
}

WorldGenTask::~WorldGenTask()
{
    cancel = true;
    if (worker.joinable()) worker.join();
}

bool WorldGenTask::isFinished() const
{
    return finished;
}

float WorldGenTask::getProgress() const
{
    return progress;
}

void WorldGenTask::update()
{
    if (finished.exchange(false))
    {
        onFinished.emit();
    }
}

void WorldGenTask::cancelTask()
{
    cancel = true;
}

std::unique_ptr<World> WorldGenTask::takeResult()
{
    std::lock_guard lock(mtx);
    return std::move(result);
}
