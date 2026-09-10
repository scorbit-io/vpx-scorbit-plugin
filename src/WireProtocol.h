// license:GPLv3+

#pragma once

// Wire protocol 1.0 between the Scorbit daemon and this plugin.
//
// The daemon listens on a user-scoped local socket and the plugin connects. On
// the wire every integer is little-endian and a string is a u16 byte count
// followed by UTF-8 bytes. A message is
//
//    u32 length   bytes that follow this field
//    u16 type
//    u16 flags    bit 0 response, bit 1 error
//    u32 seq      a response echoes the sequence of its request
//    payload
//
// The normative layout is scorbitd's
// docs/openspec/specs/vpx-virtual-probe/design.md, subsection "Message layouts
// (protocol 1.0, slice 1)", at revision 3e8a9d5. This file implements that
// subsection and nothing else. It is deliberately free of any Visual Pinball or
// Scorbit SDK dependency so the codec can be unit tested on its own, and so the
// daemon's SocketCable and this plugin can each be diffed against the spec
// rather than against each other.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Scorbit::Wire
{

inline constexpr uint16_t PROTO_MAJOR = 1;
inline constexpr uint16_t PROTO_MINOR = 0;

// Largest message accepted from the peer, counted from the first byte of the
// length field. Sends are held one header below it so a peer that measures the
// bound either way accepts everything this side produces.
inline constexpr uint32_t MAX_MESSAGE_BYTES = 1u << 20;
inline constexpr uint32_t HEADER_BYTES = 8;  // type, flags, seq
inline constexpr uint32_t LENGTH_BYTES = 4;
inline constexpr uint32_t MAX_PAYLOAD_BYTES = MAX_MESSAGE_BYTES - LENGTH_BYTES - HEADER_BYTES;

enum MsgType : uint16_t
{
   TYPE_HELLO = 1,
   TYPE_HELLO_ACK = 2,
   TYPE_DECLARE = 3,
   TYPE_FRAME = 4,
   TYPE_PING = 5,
   TYPE_PONG = 6,
   TYPE_BYE = 7,
   // Reserved for later slices. A type id this side does not implement is
   // answered with unsupported_type whether it is unknown or merely reserved,
   // because from a 1.0 peer's point of view it does not exist yet. Answered
   // rather than ignored, so the daemon learns at once what this build serves.
   TYPE_SUBSCRIBE = 16,
   TYPE_POLL = 17,
   TYPE_READ_DIRECT = 18,
   TYPE_WRITE = 19,
   TYPE_NVRAM = 20,
   TYPE_OVERLAY = 32,
   TYPE_STATUS = 33,
};

enum MsgFlags : uint16_t
{
   FLAG_RESPONSE = 1u << 0,
   FLAG_ERROR = 1u << 1,
};

// The numbered table in design.md is the single definition, names and numbers
// together. Codes 8 to 12 name conditions only the slice 2 messages can raise;
// they are carried here anyway so the two lists cannot drift apart.
enum ErrorCode : uint16_t
{
   ERR_UNSUPPORTED_TYPE = 1,
   ERR_BAD_TOKEN = 2,
   ERR_VERSION_MISMATCH = 3,
   ERR_BUSY = 4,
   ERR_NOT_DECLARED = 5,
   ERR_UNSUPPORTED_OPERATION = 6,
   ERR_MALFORMED = 7,
   ERR_NO_GAME = 8,
   ERR_OUT_OF_RANGE = 9,
   // Slice 2 only, for an over-sized subscription. An over-sized wire frame is
   // ERR_MALFORMED, not this.
   ERR_TOO_LARGE = 10,
   ERR_STALE = 11,
   ERR_UNSTABLE = 12,
};

// Hello capability bits. Slice 1 offers identify frames and nothing else.
enum Capability : uint32_t
{
   CAP_IDENTIFY_FRAMES = 1u << 0,
   CAP_MEMORY_READ = 1u << 1,
   CAP_MEMORY_WRITE = 1u << 2,
   CAP_MEMORY_BATCH_READ = 1u << 3,
   CAP_HALT = 1u << 4,
   CAP_OVERLAY = 1u << 5,
   CAP_STATES = 1u << 6,
};

const char* TypeName(uint16_t type);
const char* ErrorName(uint16_t code);

// Little-endian payload writer. Overflowing a string length or the payload
// bound latches a failure rather than truncating silently.
class Writer final
{
public:
   void U8(uint8_t v);
   void U16(uint16_t v);
   void U32(uint32_t v);
   void U64(uint64_t v);
   void Str(const std::string& v);
   void Bytes(const uint8_t* data, size_t size);

   bool Ok() const { return m_ok; }
   const std::vector<uint8_t>& Data() const { return m_data; }
   std::vector<uint8_t> Take() { return std::move(m_data); }

private:
   std::vector<uint8_t> m_data;
   bool m_ok = true;
};

// Little-endian payload reader. Every accessor returns false and latches a
// failure when the buffer is short, so a truncated message is rejected at the
// first field that does not fit rather than read as zeroes.
class Reader final
{
public:
   Reader(const uint8_t* data, size_t size)
      : m_data(data)
      , m_size(size)
   {
   }

   bool U8(uint8_t& v);
   bool U16(uint16_t& v);
   bool U32(uint32_t& v);
   bool U64(uint64_t& v);
   bool Str(std::string& v);
   bool Bytes(std::vector<uint8_t>& v, size_t count);

   bool Ok() const { return m_ok; }
   size_t Remaining() const { return m_size - m_pos; }
   bool AtEnd() const { return m_pos == m_size; }

private:
   bool Need(size_t count);

   const uint8_t* m_data;
   size_t m_size;
   size_t m_pos = 0;
   bool m_ok = true;
};

struct Header
{
   uint16_t type = 0;
   uint16_t flags = 0;
   uint32_t seq = 0;

   // An error is a response. This side always sets both bits when it sends
   // one, and the spec requires a reader to accept an error carrying only
   // bit 1, so classification tests either bit. Without that, a bit 1 only
   // error would be taken for a request and answered with nonsense.
   bool IsResponse() const { return (flags & (FLAG_RESPONSE | FLAG_ERROR)) != 0; }
   bool IsError() const { return (flags & FLAG_ERROR) != 0; }
};

struct Message
{
   Header header;
   std::vector<uint8_t> payload;
};

// Frames a payload for the socket. Returns an empty vector when the payload
// exceeds MAX_PAYLOAD_BYTES; the caller must treat that as a send failure.
std::vector<uint8_t> EncodeMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload);

// Splits a message body (everything the length field counted) into header and
// payload. False when the body is shorter than a header.
bool DecodeBody(const uint8_t* body, size_t size, Message& out);

// --- Payloads ------------------------------------------------------------

struct Hello
{
   uint16_t protoMajor = PROTO_MAJOR;
   uint16_t protoMinor = PROTO_MINOR;
   std::string pluginVersion;
   std::string vpxRevision;
   std::string pluginApiCommit;
   std::string token;          // 64 hex characters
   uint64_t instanceId = 0;    // random, once per plugin load
   uint32_t capabilities = 0;
   std::string info;           // JSON or empty

   bool operator==(const Hello&) const = default;
};

struct HelloAck
{
   uint16_t protoMajor = 0;
   uint16_t protoMinor = 0;
   std::string daemonVersion;
   uint32_t capabilities = 0;
   uint32_t sessionId = 0;

   bool operator==(const HelloAck&) const = default;
};

struct Declare
{
   std::string romId;
   std::string tablePath;
   std::string vpxVersion;
   std::string vpxRevision;
   uint16_t width = 0;
   uint16_t height = 0;
   uint8_t shades = 0;              // 4 or 16, 0 when there is no conforming source
   uint32_t sourceGeneration = 0;
   std::string identifyFormat;      // "BITPLANE2", "BITPLANE4" or empty

   bool operator==(const Declare&) const = default;
};

// Reserved since_frame_id: the requester holds no frame at all, so the reply
// must carry pixels whatever since_generation says.
//
// A sentinel is needed because no generation value can stand in for one. The
// daemon used to force a full reply by asking with a generation it believed
// impossible, and 0xFFFFFFFF is not impossible: paired with a real frame id of
// 0 it names a frame that can genuinely exist, and the plugin would answer
// "unchanged" to a daemon holding nothing.
//
// The cost is one redundant full frame if a display source ever reaches frame
// id 0xFFFFFFFF, which at sixty frames a second is years of continuous play.
inline constexpr uint32_t SINCE_FRAME_NONE = 0xFFFFFFFFu;

struct FrameRequest
{
   uint32_t sinceFrameId = 0;
   uint32_t sinceGeneration = 0;

   bool operator==(const FrameRequest&) const = default;
};

struct FrameReply
{
   uint32_t frameId = 0;
   uint32_t sourceGeneration = 0;
   uint16_t width = 0;
   uint16_t height = 0;
   uint8_t shades = 0;
   uint8_t hasPixels = 0;
   std::vector<uint8_t> pixels;     // width * height shade indices, row major

   bool operator==(const FrameReply&) const = default;
};

struct Pong
{
   uint64_t renderedFrame = 0;
   uint8_t playerRunning = 0;
   uint8_t writesSupported = 0;

   bool operator==(const Pong&) const = default;
};

struct Bye
{
   std::string reason;

   bool operator==(const Bye&) const = default;
};

struct ErrorPayload
{
   uint16_t code = 0;
   std::string reason;

   bool operator==(const ErrorPayload&) const = default;
};

std::vector<uint8_t> Encode(const Hello& v);
std::vector<uint8_t> Encode(const HelloAck& v);
std::vector<uint8_t> Encode(const Declare& v);
std::vector<uint8_t> Encode(const FrameRequest& v);
std::vector<uint8_t> Encode(const FrameReply& v);
std::vector<uint8_t> Encode(const Pong& v);
std::vector<uint8_t> Encode(const Bye& v);
std::vector<uint8_t> Encode(const ErrorPayload& v);

// Every Decode requires each field it declares to be present and ignores any
// bytes trailing them. That is the compatibility rule: appending a field is a
// minor bump, so a 1.0 reader must not refuse a 1.1 payload it can still read.
bool Decode(const std::vector<uint8_t>& payload, Hello& out);
bool Decode(const std::vector<uint8_t>& payload, HelloAck& out);
bool Decode(const std::vector<uint8_t>& payload, Declare& out);
bool Decode(const std::vector<uint8_t>& payload, FrameRequest& out);
bool Decode(const std::vector<uint8_t>& payload, FrameReply& out);
bool Decode(const std::vector<uint8_t>& payload, Pong& out);
bool Decode(const std::vector<uint8_t>& payload, Bye& out);
bool Decode(const std::vector<uint8_t>& payload, ErrorPayload& out);

}
