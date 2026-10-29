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
   case TYPE_GAME: return "Game";
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
   case ERR_NOT_AUTHORED: return "not_authored";
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

std::vector<uint8_t> Encode(const Declare& v, uint16_t peerMinor, std::vector<SensitiveSpan>* sensitive)
{
   if (sensitive != nullptr)
      sensitive->clear();
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
   if (peerMinor >= 1)
   {
      w.U8(v.tableKind);
      w.Str(v.machineUuid);
      for (const std::string* path : { &v.tableFullPath, &v.vpxAppPath, &v.prefsPath })
      {
         if (sensitive != nullptr && !path->empty())
            sensitive->push_back({ w.Data().size() + 2, path->size() });
         w.Str(*path);
      }
   }
   if (!w.Ok())
   {
      if (sensitive != nullptr)
         sensitive->clear();
      return { };
   }
   return w.Take();
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
   out.tableKind = TABLE_ROM;
   out.machineUuid.clear();
   out.tableFullPath.clear();
   out.vpxAppPath.clear();
   out.prefsPath.clear();
   if (r.Ok() && !r.AtEnd())
   {
      r.U8(out.tableKind);
      r.Str(out.machineUuid);
      r.Str(out.tableFullPath);
      r.Str(out.vpxAppPath);
      r.Str(out.prefsPath);
   }
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

std::vector<uint8_t> Encode(const Subscription& v)
{
   if (v.ranges.size() > 0xFFFF)
      return { };
   Writer w;
   w.U16(static_cast<uint16_t>(v.ranges.size()));
   for (const MemoryRange& range : v.ranges)
   {
      w.U32(range.address);
      w.U32(range.length);
   }
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Subscription& out)
{
   Reader r(payload.data(), payload.size());
   uint16_t count = 0;
   out.ranges.clear();
   if (!r.U16(count))
      return false;
   // Checked before reserving, so a hostile count cannot make us allocate.
   if (r.Remaining() < static_cast<size_t>(count) * 8)
      return false;
   out.ranges.resize(count);
   for (MemoryRange& range : out.ranges)
   {
      r.U32(range.address);
      r.U32(range.length);
   }
   return r.Ok();
}

std::vector<uint8_t> Encode(const PollReply& v)
{
   if (v.blocks.size() > 0xFFFF)
      return { };
   Writer w;
   w.U64(v.frame);
   w.U8(v.playerRunning);
   w.U16(static_cast<uint16_t>(v.blocks.size()));
   for (const MemoryBlock& block : v.blocks)
   {
      w.U32(block.address);
      w.U32(static_cast<uint32_t>(block.bytes.size()));
      w.Bytes(block.bytes.data(), block.bytes.size());
   }
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, PollReply& out)
{
   Reader r(payload.data(), payload.size());
   uint16_t count = 0;
   out.blocks.clear();
   r.U64(out.frame);
   r.U8(out.playerRunning);
   if (!r.U16(count))
      return false;
   for (uint16_t i = 0; i < count && r.Ok(); i++)
   {
      MemoryBlock block;
      uint32_t length = 0;
      r.U32(block.address);
      if (!r.U32(length) || length > r.Remaining())
         return false;
      r.Bytes(block.bytes, length);
      out.blocks.push_back(std::move(block));
   }
   return r.Ok();
}

std::vector<uint8_t> Encode(const ReadDirectRequest& v)
{
   Writer w;
   w.U32(v.address);
   w.U32(v.length);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, ReadDirectRequest& out)
{
   Reader r(payload.data(), payload.size());
   r.U32(out.address);
   r.U32(out.length);
   return r.Ok();
}

std::vector<uint8_t> Encode(const ReadDirectReply& v)
{
   if (v.unstableOffsets.size() > 0xFFFF)
      return { };
   Writer w;
   w.U64(v.frame);
   w.U32(v.address);
   w.U32(static_cast<uint32_t>(v.bytes.size()));
   w.U16(static_cast<uint16_t>(v.unstableOffsets.size()));
   for (const uint32_t offset : v.unstableOffsets)
      w.U32(offset);
   w.Bytes(v.bytes.data(), v.bytes.size());
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, ReadDirectReply& out)
{
   Reader r(payload.data(), payload.size());
   uint32_t length = 0;
   uint16_t unstable = 0;
   out.unstableOffsets.clear();
   out.bytes.clear();
   r.U64(out.frame);
   r.U32(out.address);
   r.U32(length);
   if (!r.U16(unstable) || r.Remaining() < static_cast<size_t>(unstable) * 4)
      return false;
   out.unstableOffsets.resize(unstable);
   for (uint32_t& offset : out.unstableOffsets)
      r.U32(offset);
   if (!r.Ok() || length > r.Remaining())
      return false;
   r.Bytes(out.bytes, length);
   return r.Ok();
}


std::vector<uint8_t> Encode(const WriteRequest& v)
{
   if (v.ops.size() > 0xFFFF)
      return { };
   Writer w;
   w.U16(static_cast<uint16_t>(v.ops.size()));
   for (const WriteOp& op : v.ops)
   {
      if (op.bytes.size() > 0xFFFF)
         return { };
      w.U32(op.address);
      w.U16(static_cast<uint16_t>(op.bytes.size()));
      w.Bytes(op.bytes.data(), op.bytes.size());
   }
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, WriteRequest& out)
{
   Reader r(payload.data(), payload.size());
   uint16_t count = 0;
   out.ops.clear();
   if (!r.U16(count))
      return false;
   // Each op is at least 6 bytes, so a count the payload cannot hold is refused before allocating.
   if (r.Remaining() < static_cast<size_t>(count) * 6)
      return false;
   out.ops.reserve(count);
   for (uint16_t i = 0; i < count; i++)
   {
      WriteOp op;
      uint16_t length = 0;
      r.U32(op.address);
      if (!r.U16(length) || length > r.Remaining())
         return false;
      r.Bytes(op.bytes, length);
      out.ops.push_back(std::move(op));
   }
   return r.Ok();
}

std::vector<uint8_t> Encode(const WriteReply& v)
{
   Writer w;
   w.U64(v.frame);
   w.U16(v.applied);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, WriteReply& out)
{
   Reader r(payload.data(), payload.size());
   r.U64(out.frame);
   r.U16(out.applied);
   return r.Ok();
}

std::vector<uint8_t> Encode(const NvRamParameters& v)
{
   Writer w;
   w.U32(v.start);
   w.U32(v.length);
   w.U32(v.unlockAddress);
   w.U8(v.unlockValue);
   w.U8(v.lockValue);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, NvRamParameters& out)
{
   Reader r(payload.data(), payload.size());
   r.U32(out.start);
   r.U32(out.length);
   r.U32(out.unlockAddress);
   r.U8(out.unlockValue);
   r.U8(out.lockValue);
   return r.Ok();
}

std::vector<uint8_t> Encode(const OverlayRequest& v)
{
   Writer w;
   w.U8(v.op);
   w.Bytes(v.payload.data(), v.payload.size());
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, OverlayRequest& out)
{
   Reader r(payload.data(), payload.size());
   if (!r.U8(out.op))
      return false;
   out.payload.assign(payload.begin() + 1, payload.end());
   return true;
}

std::vector<uint8_t> Encode(const OverlayControl& v)
{
   Writer w;
   w.U8(v.control);
   w.U8(static_cast<uint8_t>(v.durationMs >> 8));
   w.U8(static_cast<uint8_t>(v.durationMs & 0xFF));
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, OverlayControl& out)
{
   Reader r(payload.data(), payload.size());
   uint8_t hi = 0, lo = 0;
   r.U8(out.control);
   r.U8(hi);
   r.U8(lo);
   out.durationMs = static_cast<uint16_t>((hi << 8) | lo);
   return r.Ok();
}

std::vector<uint8_t> Encode(const Status& v)
{
   Writer w;
   w.U8(v.sessionState);
   w.U8(v.systemState);
   w.Str(v.supportText);
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, Status& out)
{
   Reader r(payload.data(), payload.size());
   r.U8(out.sessionState);
   r.U8(out.systemState);
   r.Str(out.supportText);
   return r.Ok() && out.supportText.size() <= MAX_SUPPORT_TEXT_BYTES;
}

namespace
{

bool KnownGameOp(uint8_t kind)
{
   return kind >= GAME_START && kind <= GAME_CLEAR_MODES;
}

// The rules a batch must meet on both sides: the daemon answers malformed otherwise.
bool ValidGameBatch(const GameBatch& v)
{
   if (v.ops.empty() || v.ops.size() > MAX_GAME_OPS)
      return false;
   for (size_t i = 0; i < v.ops.size(); i++)
   {
      const uint8_t kind = v.ops[i].kind;
      if (!KnownGameOp(kind))
         return false;
      if (kind == GAME_START && i != 0)
         return false;
      if (kind == GAME_END && i != v.ops.size() - 1)
         return false;
   }
   return true;
}

}

std::vector<uint8_t> Encode(const GameBatch& v)
{
   if (!ValidGameBatch(v))
      return { };
   Writer w;
   w.U16(static_cast<uint16_t>(v.ops.size()));
   for (const GameOp& op : v.ops)
   {
      w.U8(op.kind);
      switch (op.kind)
      {
      case GAME_START:
      case GAME_BALL:
      case GAME_ACTIVE_PLAYER:
         w.U8(op.value);
         break;
      case GAME_SCORE:
         w.U8(op.value);
         w.U64(static_cast<uint64_t>(op.score));
         w.U32(static_cast<uint32_t>(op.feature));
         break;
      case GAME_ADD_MODE_EXPIRING:
         w.U32(op.seconds);
         w.Str(op.mode);
         break;
      case GAME_ADD_MODE:
      case GAME_MODE_COMPLETED:
      case GAME_REMOVE_MODE:
         w.Str(op.mode);
         break;
      default:
         break;
      }
   }
   return w.Ok() ? w.Take() : std::vector<uint8_t> { };
}

bool Decode(const std::vector<uint8_t>& payload, GameBatch& out)
{
   Reader r(payload.data(), payload.size());
   uint16_t count = 0;
   out.ops.clear();
   // Every op is at least its kind byte, so a count the payload cannot hold is refused first.
   if (!r.U16(count) || count == 0 || count > MAX_GAME_OPS || r.Remaining() < count)
      return false;
   out.ops.reserve(count);
   for (uint16_t i = 0; i < count && r.Ok(); i++)
   {
      GameOp op;
      r.U8(op.kind);
      switch (op.kind)
      {
      case GAME_START:
      case GAME_BALL:
      case GAME_ACTIVE_PLAYER:
         r.U8(op.value);
         break;
      case GAME_SCORE:
      {
         uint64_t score = 0;
         uint32_t feature = 0;
         r.U8(op.value);
         r.U64(score);
         r.U32(feature);
         op.score = static_cast<int64_t>(score);
         op.feature = static_cast<int32_t>(feature);
         break;
      }
      case GAME_ADD_MODE_EXPIRING:
         r.U32(op.seconds);
         r.Str(op.mode);
         break;
      case GAME_ADD_MODE:
      case GAME_MODE_COMPLETED:
      case GAME_REMOVE_MODE:
         r.Str(op.mode);
         break;
      case GAME_END:
      case GAME_CLEAR_MODES:
         break;
      default:
         // Ops carry no length, so an unknown one cannot be skipped.
         return false;
      }
      out.ops.push_back(std::move(op));
   }
   return r.Ok() && ValidGameBatch(out);
}

}
