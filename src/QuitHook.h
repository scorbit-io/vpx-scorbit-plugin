#pragma once

namespace Scorbit
{

// Calls onQuit on the main thread once the application has committed to quitting,
// before exit() starts static teardown. macOS only; a no-op elsewhere.
#ifdef __APPLE__
void InstallQuitHook(void (*onQuit)());
void RemoveQuitHook();
#else
inline void InstallQuitHook(void (*)()) { }
inline void RemoveQuitHook() { }
#endif

}
