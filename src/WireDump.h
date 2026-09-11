// license:GPLv3+

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Scorbit
{

// Records complete wire frames to a file. Off unless SCORBIT_WIRE_DUMP names a path.
//
// The daemon's .scap capture records at the IFrameSource seam, one layer above the
// socket, so it holds frame content but no message headers and no handshake at all.
// Byte literals for the daemon-side test peer therefore cannot be taken from it, and
// this worker is the only code sitting on the wire. An AF_UNIX tap would work too,
// but it moves both endpoints to a proxy and so changes the thing under test.
//
// Frames are written exactly as they cross, u32 length prefix included, so a record
// can be replayed into the decoder unchanged. File layout, all little-endian to match
// the protocol:
//
//   "SCORBITWIRE1\n"                     once, at open
//   u8   direction   '>' plugin to daemon, '<' daemon to plugin
//   u64  micros      since Open()
//   u32  length      bytes of wire frame that follow
//   u8[length]       the wire frame, length prefix + header + payload
//
// Owned by SocketWorker and touched only from the worker thread. Every record is
// flushed, so a dump left by a crashed session is still readable.
class WireDump final
{
public:
   ~WireDump();

   WireDump() = default;
   WireDump(const WireDump&) = delete;
   WireDump& operator=(const WireDump&) = delete;

   // Opens the path named by SCORBIT_WIRE_DUMP, or leaves the dump disabled when the
   // variable is unset or empty. A path that cannot be opened disables the dump rather
   // than failing the session; `error` then carries the reason, for the caller to log
   // on whichever thread it is allowed to log from.
   void Open(std::string& error);
   void Close();

   bool Enabled() const { return m_file != nullptr; }

   void Sent(const uint8_t* data, size_t size);

   // A received frame is held in two pieces: the length prefix that was read first and
   // the body that followed. They are recorded as one frame, as they arrived on the wire.
   void Received(const uint8_t* prefix, size_t prefixSize, const uint8_t* body, size_t bodySize);

private:
   void Record(uint8_t direction, const uint8_t* prefix, size_t prefixSize, const uint8_t* body, size_t bodySize);

   std::FILE* m_file = nullptr;
   uint64_t m_openMicros = 0;
};

}
