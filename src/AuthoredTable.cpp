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

#include <utility>

namespace Scorbit
{

bool CanonicalMachineId(const std::string& id, std::string& out)
{
   if (id.size() != 36)
      return false;
   std::string lower(id);
   for (size_t i = 0; i < lower.size(); i++)
   {
      char& c = lower[i];
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
         if (c != '-')
            return false;
      }
      else if (c >= 'A' && c <= 'F')
         c = static_cast<char>(c - 'A' + 'a');
      else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   out = std::move(lower);
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
