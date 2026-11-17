// license:GPLv3+

#pragma once

// The table-script binding a release of the plugin bundles (scorbit-<version>.vbs), added to
// VPX's <prefPath>/scripts so a table can load it with GetTextFile. Add-only: a file of the same
// name is left as it is, whatever its bytes, and other versions are never touched, since tables
// written against them still load them.

#include <filesystem>
#include <string>

namespace Scorbit
{

enum class BindingInstall
{
   Added,    // copied in
   Present,  // a file of that name was already there, left untouched
   Failed,   // nothing changed; detail says why
};

struct BindingInstallResult
{
   BindingInstall outcome = BindingInstall::Failed;
   std::string detail;
};

// Never throws. The copy is written beside its destination and then linked in without replacing,
// so a crash leaves no partial file under the binding's name.
BindingInstallResult InstallBinding(const std::filesystem::path& source, const std::filesystem::path& scriptsDir);

}
