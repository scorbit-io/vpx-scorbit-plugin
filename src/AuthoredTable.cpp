// license:GPLv3+

#include "AuthoredTable.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Scorbit
{

bool ValidMachineId(const std::string& id)
{
   if (id.empty() || id.size() > 64)
      return false;
   for (const char c : id)
      if (c < 0x21 || c > 0x7E)
         return false;
   return true;
}

std::string NativePathToUtf8(const char* path)
{
   if (path == nullptr)
      return { };
#ifdef _WIN32
   const int wide = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
   if (wide <= 1)
      return { };
   std::wstring w(static_cast<size_t>(wide), L'\0');
   MultiByteToWideChar(CP_ACP, 0, path, -1, w.data(), wide);
   const int narrow = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
   if (narrow <= 1)
      return { };
   std::string utf8(static_cast<size_t>(narrow), '\0');
   WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8.data(), narrow, nullptr, nullptr);
   utf8.pop_back(); // the terminator both calls counted
   return utf8;
#else
   return path;
#endif
}

}
