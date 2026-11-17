// license:GPLv3+

#pragma once

#include <filesystem>

namespace Scorbit
{

// The folder the plugin's library was loaded from, where the files staged beside it sit; empty if
// unknown. The plugin API gives a plugin's folder to VPX only. Kept out of other sources so
// windows.h's macros reach nothing else.
std::filesystem::path PluginFolder();

}
