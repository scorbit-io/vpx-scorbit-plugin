// license:GPLv3+

#include "WireDump.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>

namespace Scorbit
{

namespace
{

constexpr char MAGIC[] = "SCORBITWIRE1\n";

uint64_t NowMicros()
{
   const auto now = std::chrono::steady_clock::now().time_since_epoch();
   return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Written explicitly rather than by copying the native representation, so a dump
// taken on one platform reads the same on another.
void PutU32(uint8_t* dst, uint32_t v)
{
   dst[0] = static_cast<uint8_t>(v);
   dst[1] = static_cast<uint8_t>(v >> 8);
   dst[2] = static_cast<uint8_t>(v >> 16);
   dst[3] = static_cast<uint8_t>(v >> 24);
}

void PutU64(uint8_t* dst, uint64_t v)
{
   for (int i = 0; i < 8; ++i)
      dst[i] = static_cast<uint8_t>(v >> (8 * i));
}

}

WireDump::~WireDump()
{
   Close();
}

void WireDump::Open(std::string& error)
{
   error.clear();
   if (m_file != nullptr)
      return;

   const char* path = getenv("SCORBIT_WIRE_DUMP");
   if (path == nullptr || path[0] == '\0')
      return;

   m_file = std::fopen(path, "wb");
   if (m_file == nullptr)
   {
      error = std::string("Socket: cannot open the wire dump at ") + path + ", continuing without it";
      return;
   }

   std::fwrite(MAGIC, 1, sizeof(MAGIC) - 1, m_file);
   std::fflush(m_file);
   m_openMicros = NowMicros();
}

void WireDump::Close()
{
   if (m_file == nullptr)
      return;
   std::fclose(m_file);
   m_file = nullptr;
}

void WireDump::Redact(const std::vector<SensitiveSpan>& sensitive)
{
   const size_t size = m_redacted.size();
   for (const SensitiveSpan& span : sensitive)
   {
      if (span.offset >= size)
         continue;
      const size_t end = span.offset + std::min(span.length, size - span.offset);
      std::fill(m_redacted.begin() + static_cast<std::ptrdiff_t>(span.offset),
         m_redacted.begin() + static_cast<std::ptrdiff_t>(end), static_cast<uint8_t>('*'));
   }
}

void WireDump::Sent(const uint8_t* data, size_t size, const std::vector<SensitiveSpan>& sensitive)
{
   if (m_file == nullptr)
      return;
   if (sensitive.empty())
   {
      Record('>', data, size, nullptr, 0);
      return;
   }
   m_redacted.assign(data, data + size);
   Redact(sensitive);
   Record('>', m_redacted.data(), m_redacted.size(), nullptr, 0);
}

void WireDump::Received(const uint8_t* prefix, size_t prefixSize, const uint8_t* body, size_t bodySize,
   const std::vector<SensitiveSpan>& sensitive)
{
   if (m_file == nullptr)
      return;
   if (sensitive.empty())
   {
      Record('<', prefix, prefixSize, body, bodySize);
      return;
   }
   m_redacted.assign(prefix, prefix + prefixSize);
   m_redacted.insert(m_redacted.end(), body, body + bodySize);
   Redact(sensitive);
   Record('<', m_redacted.data(), m_redacted.size(), nullptr, 0);
}

void WireDump::Record(uint8_t direction, const uint8_t* prefix, size_t prefixSize, const uint8_t* body, size_t bodySize)
{
   if (m_file == nullptr)
      return;

   uint8_t header[13];
   header[0] = direction;
   PutU64(header + 1, NowMicros() - m_openMicros);
   PutU32(header + 9, static_cast<uint32_t>(prefixSize + bodySize));

   std::fwrite(header, 1, sizeof(header), m_file);
   if (prefixSize != 0)
      std::fwrite(prefix, 1, prefixSize, m_file);
   if (bodySize != 0)
      std::fwrite(body, 1, bodySize, m_file);

   // Flushed per record so a dump left behind by a crashed session is still readable
   // up to the last complete frame. This is debug-only and off by default, so the
   // cost does not reach a normal run.
   std::fflush(m_file);
}

}
