// license:GPLv3+

#include "WireProtocol.h"

#include <cstring>

namespace Scorbit::Wire
{

const char* TypeName(uint16_t type)
{
   switch (type)
   {
   case TYPE_HELLO: return "Hello";
   case TYPE_HELLO_ACK: return "HelloAck";
   case TYPE_DECLARE: return "Declare";
   case TYPE_FRAME: return "Frame";
   case TYPE_PING: return "Ping";
   case TYPE_PONG: return "Pong";
   case TYPE_BYE: return "Bye";
   case TYPE_SUBSCRIBE: return "Subscribe";
   case TYPE_POLL: return "Poll";
   case TYPE_READ_DIRECT: return "ReadDirect";
   case TYPE_WRITE: return "Write";
   case TYPE_NVRAM: return "NvRam";
   case TYPE_OVERLAY: return "Overlay";
   case TYPE_STATUS: return "Status";
   default: return "unknown";
   }
}

const char* ErrorName(uint16_t code)
{
   switch (code)
   {
   case ERR_UNSUPPORTED_TYPE: return "unsupported_type";
   case ERR_BAD_TOKEN: return "bad_token";
   case ERR_VERSION_MISMATCH: return "version_mismatch";
   case ERR_BUSY: return "busy";
   case ERR_NOT_DECLARED: return "not_declared";
   case ERR_UNSUPPORTED_OPERATION: return "unsupported_operation";
   case ERR_MALFORMED: return "malformed";
   case ERR_NO_GAME: return "no_game";
   case ERR_OUT_OF_RANGE: return "out_of_range";
   case ERR_TOO_LARGE: return "too_large";
   case ERR_STALE: return "stale";
   case ERR_UNSTABLE: return "unstable";
   default: return "unknown";
   }
}

// --- Writer --------------------------------------------------------------

void Writer::U8(uint8_t v)
{
   if (!m_ok)
      return;
   if (m_data.size() + 1 > MAX_PAYLOAD_BYTES)
   {
      m_ok = false;
      return;
   }
   m_data.push_back(v);
}

void Writer::U16(uint16_t v)
{
   U8(static_cast<uint8_t>(v & 0xFF));
   U8(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void Writer::U32(uint32_t v)
{
   U16(static_cast<uint16_t>(v & 0xFFFF));
   U16(static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

void Writer::U64(uint64_t v)
{
   U32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
   U32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void Writer::Str(const std::string& v)
{
   if (!m_ok)
      return;
   if (v.size() > 0xFFFF)
   {
      m_ok = false;
      return;
   }
   U16(static_cast<uint16_t>(v.size()));
   Bytes(reinterpret_cast<const uint8_t*>(v.data()), v.size());
}

void Writer::Bytes(const uint8_t* data, size_t size)
{
   if (!m_ok)
      return;
   if (m_data.size() + size > MAX_PAYLOAD_BYTES)
   {
      m_ok = false;
      return;
   }
   if (size != 0)
      m_data.insert(m_data.end(), data, data + size);
}

// --- Reader --------------------------------------------------------------

bool Reader::Need(size_t count)
{
   if (!m_ok)
      return false;
   if (m_size - m_pos < count)
   {
      m_ok = false;
      return false;
   }
   return true;
}

bool Reader::U8(uint8_t& v)
{
   if (!Need(1))
      return false;
   v = m_data[m_pos++];
   return true;
}

bool Reader::U16(uint16_t& v)
{
   if (!Need(2))
      return false;
   v = static_cast<uint16_t>(m_data[m_pos] | (static_cast<uint16_t>(m_data[m_pos + 1]) << 8));
   m_pos += 2;
   return true;
}

bool Reader::U32(uint32_t& v)
{
   if (!Need(4))
      return false;
   v = static_cast<uint32_t>(m_data[m_pos])
      | (static_cast<uint32_t>(m_data[m_pos + 1]) << 8)
      | (static_cast<uint32_t>(m_data[m_pos + 2]) << 16)
      | (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
   m_pos += 4;
   return true;
}

bool Reader::U64(uint64_t& v)
{
   uint32_t lo = 0, hi = 0;
   if (!U32(lo) || !U32(hi))
      return false;
   v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
   return true;
}

bool Reader::Str(std::string& v)
{
   uint16_t n = 0;
   if (!U16(n))
      return false;
   if (!Need(n))
      return false;
   v.assign(reinterpret_cast<const char*>(m_data + m_pos), n);
   m_pos += n;
   return true;
}

bool Reader::Bytes(std::vector<uint8_t>& v, size_t count)
{
   if (!Need(count))
      return false;
   v.assign(m_data + m_pos, m_data + m_pos + count);
   m_pos += count;
   return true;
}

// --- Framing -------------------------------------------------------------

std::vector<uint8_t> EncodeMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload)
{
   if (payload.size() > MAX_PAYLOAD_BYTES)
      return { };

   // Written by hand rather than through Writer: Writer's bound is the payload
   // bound, and the framed message is one header longer than that by design.
   std::vector<uint8_t> out;
   out.reserve(LENGTH_BYTES + HEADER_BYTES + payload.size());
   const auto u16 = [&out](uint16_t v)
   {
      out.push_back(static_cast<uint8_t>(v & 0xFF));
      out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
   };
   const auto u32 = [&u16](uint32_t v)
   {
      u16(static_cast<uint16_t>(v & 0xFFFF));
      u16(static_cast<uint16_t>((v >> 16) & 0xFFFF));
   };
   u32(static_cast<uint32_t>(HEADER_BYTES + payload.size()));
   u16(type);
   u16(flags);
   u32(seq);
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

bool DecodeBody(const uint8_t* body, size_t size, Message& out)
{
   if (size < HEADER_BYTES)
      return false;
   Reader r(body, size);
   uint16_t type = 0, flags = 0;
   uint32_t seq = 0;
   if (!r.U16(type) || !r.U16(flags) || !r.U32(seq))
      return false;
   out.header.type = type;
   out.header.flags = flags;
   out.header.seq = seq;
   out.payload.assign(body + HEADER_BYTES, body + size);
   return true;
}

// --- Payloads ------------------------------------------------------------

std::vector<uint8_t> Encode(const Hello& v)
{
   Writer w;
   w.U16(v.protoMajor);
   w.U16(v.protoMinor);
   w.Str(v.pluginVersion);
   w.Str(v.vpxRevision);
   w.Str(v.pluginApiCommit);
   w.Str(v.token);
   w.U64(v.instanceId);
   w.U32(v.capabilities);
   w.Str(v.info);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Hello& out)
{
   Reader r(payload.data(), payload.size());
   r.U16(out.protoMajor);
   r.U16(out.protoMinor);
   r.Str(out.pluginVersion);
   r.Str(out.vpxRevision);
   r.Str(out.pluginApiCommit);
   r.Str(out.token);
   r.U64(out.instanceId);
   r.U32(out.capabilities);
   r.Str(out.info);
   return r.Ok();
}

std::vector<uint8_t> Encode(const HelloAck& v)
{
   Writer w;
   w.U16(v.protoMajor);
   w.U16(v.protoMinor);
   w.Str(v.daemonVersion);
   w.U32(v.capabilities);
   w.U32(v.sessionId);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, HelloAck& out)
{
   Reader r(payload.data(), payload.size());
   r.U16(out.protoMajor);
   r.U16(out.protoMinor);
   r.Str(out.daemonVersion);
   r.U32(out.capabilities);
   r.U32(out.sessionId);
   return r.Ok();
}

std::vector<uint8_t> Encode(const Declare& v)
{
   Writer w;
   w.Str(v.romId);
   w.Str(v.tablePath);
   w.Str(v.vpxVersion);
   w.Str(v.vpxRevision);
   w.U16(v.width);
   w.U16(v.height);
   w.U8(v.shades);
   w.U32(v.sourceGeneration);
   w.Str(v.identifyFormat);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Declare& out)
{
   Reader r(payload.data(), payload.size());
   r.Str(out.romId);
   r.Str(out.tablePath);
   r.Str(out.vpxVersion);
   r.Str(out.vpxRevision);
   r.U16(out.width);
   r.U16(out.height);
   r.U8(out.shades);
   r.U32(out.sourceGeneration);
   r.Str(out.identifyFormat);
   return r.Ok();
}

std::vector<uint8_t> Encode(const FrameRequest& v)
{
   Writer w;
   w.U32(v.sinceFrameId);
   w.U32(v.sinceGeneration);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, FrameRequest& out)
{
   Reader r(payload.data(), payload.size());
   r.U32(out.sinceFrameId);
   r.U32(out.sinceGeneration);
   return r.Ok();
}

std::vector<uint8_t> Encode(const FrameReply& v)
{
   Writer w;
   w.U32(v.frameId);
   w.U32(v.sourceGeneration);
   w.U16(v.width);
   w.U16(v.height);
   w.U8(v.shades);
   w.U8(v.hasPixels);
   if (v.hasPixels != 0)
   {
      // The pixel run is implied by the geometry, never length prefixed, so a
      // reply whose vector disagrees with its header would decode as garbage
      // on the far side. Refuse to encode it instead.
      if (v.pixels.size() != static_cast<size_t>(v.width) * v.height)
         return { };
      w.Bytes(v.pixels.data(), v.pixels.size());
   }
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, FrameReply& out)
{
   Reader r(payload.data(), payload.size());
   r.U32(out.frameId);
   r.U32(out.sourceGeneration);
   r.U16(out.width);
   r.U16(out.height);
   r.U8(out.shades);
   r.U8(out.hasPixels);
   out.pixels.clear();
   if (r.Ok() && out.hasPixels != 0)
      r.Bytes(out.pixels, static_cast<size_t>(out.width) * out.height);
   return r.Ok();
}

std::vector<uint8_t> Encode(const Pong& v)
{
   Writer w;
   w.U64(v.renderedFrame);
   w.U8(v.playerRunning);
   w.U8(v.writesSupported);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Pong& out)
{
   Reader r(payload.data(), payload.size());
   r.U64(out.renderedFrame);
   r.U8(out.playerRunning);
   r.U8(out.writesSupported);
   return r.Ok();
}

std::vector<uint8_t> Encode(const Bye& v)
{
   Writer w;
   w.Str(v.reason);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Bye& out)
{
   Reader r(payload.data(), payload.size());
   r.Str(out.reason);
   return r.Ok();
}

std::vector<uint8_t> Encode(const ErrorPayload& v)
{
   Writer w;
   w.U16(v.code);
   w.Str(v.reason);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, ErrorPayload& out)
{
   Reader r(payload.data(), payload.size());
   r.U16(out.code);
   r.Str(out.reason);
   return r.Ok();
}

}
