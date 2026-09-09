// license:GPLv3+

#pragma once

// The smallest thing that counts as a test framework: a counter, a macro and a
// non zero exit code. The plugin has no test dependency and this keeps it that
// way.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ScorbitTest
{

inline int g_failures = 0;
inline int g_checks = 0;

inline void Report(bool ok, const char* expr, const char* file, int line, const std::string& note)
{
   g_checks++;
   if (ok)
      return;
   g_failures++;
   fprintf(stderr, "FAIL %s:%d: %s%s\n", file, line, expr, note.empty() ? "" : (" - " + note).c_str());
}

inline int Summary(const char* name)
{
   fprintf(stderr, "%s: %d checks, %d failures\n", name, g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}

inline std::string Hex(const std::vector<uint8_t>& bytes)
{
   static const char* digits = "0123456789abcdef";
   std::string out;
   out.reserve(bytes.size() * 2);
   for (const uint8_t b : bytes)
   {
      out.push_back(digits[b >> 4]);
      out.push_back(digits[b & 0x0F]);
   }
   return out;
}

inline std::vector<uint8_t> Unhex(const std::string& hex)
{
   auto nibble = [](char c) -> uint8_t
   {
      if (c >= '0' && c <= '9')
         return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f')
         return static_cast<uint8_t>(c - 'a' + 10);
      return static_cast<uint8_t>(c - 'A' + 10);
   };
   std::vector<uint8_t> out;
   out.reserve(hex.size() / 2);
   for (size_t i = 0; i + 1 < hex.size(); i += 2)
      out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
   return out;
}

}

#define CHECK(x) ::ScorbitTest::Report(static_cast<bool>(x), #x, __FILE__, __LINE__, std::string())
#define CHECK_MSG(x, note) ::ScorbitTest::Report(static_cast<bool>(x), #x, __FILE__, __LINE__, (note))
