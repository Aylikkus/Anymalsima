#pragma once

class IPlatform {
public:
    virtual void stop() = 0;
    virtual int64_t getTimeMs() = 0;
    virtual double getFps() = 0;
};