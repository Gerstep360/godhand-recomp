#pragma once
#include <cstdint>

// Port-owned UI, always on the window thread. Startup waits pump this UI before any guest code runs.
namespace ps2x::hostui
{
    using Callback = void (*)();
    void install(Callback init, Callback startup, Callback draw, Callback shutdown, Callback tick = nullptr);
    void initialize();
    void beginStartup();
    void startupFrame();
    void endStartup();
    bool preparingOnThisThread();
    bool closing();
    void draw();
    void shutdown();
    void setInputBlocked(bool blocked);
    void shaderProgress(uint32_t done, uint32_t total);
    void shaderProgress(uint32_t &done, uint32_t &total, bool);
}
