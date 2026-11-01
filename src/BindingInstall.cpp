// license:GPLv3+

#include "BindingInstall.h"

#include <atomic>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Scorbit
{

namespace fs = std::filesystem;

namespace
{

// Gives the finished copy its name only if nothing has that name yet; true when it did.
// std::filesystem::rename replaces an existing file on POSIX, which add-only must never do.
bool LinkNoReplace(const fs::path& from, const fs::path& to, std::error_code& ec)
{
#ifdef _WIN32
   if (MoveFileExW(from.c_str(), to.c_str(), 0))
      return true;
   ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
   return false;
#else
   if (link(from.c_str(), to.c_str()) == 0)
   {
      std::error_code ignored;
      fs::remove(from, ignored);
      return true;
   }
   ec = std::error_code(errno, std::generic_category());
   return false;
#endif
}

BindingInstallResult Fail(std::string detail)
{
   return { BindingInstall::Failed, std::move(detail) };
}

}

BindingInstallResult InstallBinding(const fs::path& source, const fs::path& scriptsDir)
{
   std::error_code ec;
   if (!fs::is_regular_file(source, ec))
      return Fail("the bundled binding " + source.filename().string() + " is missing");

   const fs::path target = scriptsDir / source.filename();
   if (fs::exists(fs::symlink_status(target, ec)))
      return { BindingInstall::Present, {} };

   fs::create_directories(scriptsDir, ec);
   if (ec || !fs::is_directory(scriptsDir, ec))
      return Fail("cannot create the scripts folder: " + (ec ? ec.message() : std::string("not a folder")));

   static std::atomic<unsigned> counter { 0 };
   const fs::path temp = scriptsDir / (source.filename().string() + ".part-" + std::to_string(counter++));
   fs::remove(temp, ec);
   ec.clear();
   if (!fs::copy_file(source, temp, fs::copy_options::none, ec))
   {
      std::error_code ignored;
      fs::remove(temp, ignored);
      return Fail("cannot write to the scripts folder: " + ec.message());
   }
   if (!LinkNoReplace(temp, target, ec))
   {
      std::error_code ignored;
      fs::remove(temp, ignored);
      // Another VPX may have added it in between; then it is present, not failed.
      if (fs::exists(fs::symlink_status(target, ignored)))
         return { BindingInstall::Present, {} };
      return Fail("cannot add the binding: " + ec.message());
   }
   return { BindingInstall::Added, {} };
}

}
