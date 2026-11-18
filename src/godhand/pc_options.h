#pragma once
#include <filesystem>
class PS2Runtime;
namespace godhand::pcoptions
{
    void install(const std::filesystem::path &settings, bool headless);
    void installNativeHooks(PS2Runtime &runtime);
}
