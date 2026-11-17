// license:GPLv3+

#include "PluginFolder.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Scorbit
{

std::filesystem::path PluginFolder()
{
#ifdef _WIN32
   HMODULE self = nullptr;
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&PluginFolder), &self))
      return { };
   std::wstring path(MAX_PATH, L'\0');
   for (;;)
   {
      const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
      if (n == 0)
         return { };
      if (n < path.size())
      {
         path.resize(n);
         break;
      }
      path.resize(path.size() * 2);   // truncated: a long path
   }
   return std::filesystem::path(path).parent_path();
#else
   Dl_info info { };
   if (dladdr(reinterpret_cast<const void*>(&PluginFolder), &info) == 0 || info.dli_fname == nullptr)
      return { };
   return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

}
