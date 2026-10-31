// license:GPLv3+

#pragma once

// Wire protocol 1.2 between the Scorbit daemon and this plugin.
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
// This file is the plugin's definition of the wire protocol; the daemon implements the
// same layouts, and the golden vectors in test/WireCodecTest.cpp pin both. It is
// deliberately free of any Visual Pinball or Scorbit SDK dependency so the codec can be
// unit tested on its own.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Scorbit::Wire
{

inline constexpr uint16_t PROTO_MAJOR = 1;
inline constexpr uint16_t PROTO_MINOR = 2;

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
   // Slice 5, plugin to daemon only: one committed batch of an authored table's game calls.
   TYPE_GAME = 48,
   TYPE_GAME_START_REQUESTED = 49,   // reserved, not in 1.2
   // Slice 6 (protocol 1.2): the script's read-backs.
   TYPE_TOP_SCORES_REQUEST = 64,     // plugin to daemon
   TYPE_TOP_SCORES = 65,             // daemon to plugin, as are 66 and 67
   TYPE_PLAYERS = 66,
   TYPE_PLAYER_PICTURE = 67,
   // Reserved, not in 1.2: answered as any type this side does not implement.
   TYPE_CREDITS_ADD_REQUESTED = 68,
   TYPE_CREDITS_STATUS_REQUESTED = 69,
   TYPE_CREDITS_DROPPED = 70,
   TYPE_CREDITS_STATUS = 71,
   TYPE_PRICING = 72,
};

enum MsgFlags : uint16_t
{
   FLAG_RESPONSE = 1u << 0,
   FLAG_ERROR = 1u << 1,
};

// The numbered table in the wire protocol definition is the single definition, names and numbers
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
   // An over-sized subscription, a Game over MAX_GAME_OPS, or the daemon's Game queue
   // overflowing. An over-sized wire frame is ERR_MALFORMED, not this.
   ERR_TOO_LARGE = 10,
   ERR_STALE = 11,
   ERR_UNSTABLE = 12,
   // A Game while the current Declare is a ROM table.
   ERR_NOT_AUTHORED = 13,
};

// Hello and HelloAck capability bits.
enum Capability : uint32_t
{
   CAP_IDENTIFY_FRAMES = 1u << 0,
   CAP_MEMORY_READ = 1u << 1,
   CAP_MEMORY_WRITE = 1u << 2,
   CAP_MEMORY_BATCH_READ = 1u << 3,
   CAP_HALT = 1u << 4,
   CAP_OVERLAY = 1u << 5,
   CAP_STATES = 1u << 6,
   // Hello: the plugin has the table-script bindings. HelloAck: the daemon serves authored tables.
   CAP_AUTHORED_TABLES = 1u << 7,
   // 1.2. Hello: the script object exposes it. HelloAck: the daemon serves it.
   CAP_LEADERBOARDS = 1u << 8,
   CAP_PLAYERS = 1u << 9,
   CAP_PLAYER_PICTURES = 1u << 10,  // needs CAP_PLAYERS on both sides
   // Reserved, not in 1.2: never set.
   CAP_START_REQUESTS = 1u << 11,
   CAP_CREDITS = 1u << 12,
   CAP_PRICING = 1u << 13,
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
   size_t Size() const { return m_data.size(); }

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
   size_t Position() const { return m_pos; }

private:
   bool Need(size_t count);

   const uint8_t* m_data;
   size_t m_size;
   size_t m_pos = 0;
   bool m_ok = true;
};

// The bytes of a string field the sender marks sensitive: the UTF-8 bytes only, never the u16
// length before them, so a dump that blanks them still decodes. Offsets count from the first
// byte of the payload.
struct SensitiveSpan
{
   size_t offset = 0;
   size_t length = 0;
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

// Declare::tablePath carries a file name, never the path VPX hands over. A full path can
// name a home directory or other personal folders, and it reaches the daemon's logs and
// every wire dump taken. The
// daemon identifies a table by its ROM, so the leading directories were never load
// bearing. Both separators are cut: this plugin ships on Windows too.
inline std::string TableFileName(const std::string& path)
{
   const size_t cut = path.find_last_of("/\\");
   return cut == std::string::npos ? path : path.substr(cut + 1);
}

enum TableKind : uint8_t
{
   TABLE_ROM = 0,
   TABLE_AUTHORED = 1,
};

struct Declare
{
   std::string romId;
   std::string tablePath;           // file name only, see TableFileName above
   std::string vpxVersion;
   std::string vpxRevision;
   uint16_t width = 0;
   uint16_t height = 0;
   uint8_t shades = 0;              // 4 or 16, 0 when there is no conforming source
   uint32_t sourceGeneration = 0;
   std::string identifyFormat;      // "BITPLANE2", "BITPLANE4" or empty
   // Appended in 1.1 (slice 5). Empty for a ROM table. The three paths are full host paths:
   // never logged, and blanked in wire dumps.
   uint8_t tableKind = TABLE_ROM;
   std::string machineUuid;         // lower-case canonical uuid
   std::string tableFullPath;
   std::string vpxAppPath;
   std::string prefsPath;           // VPX's preference directory (its m_prefPath, with a trailing separator), in
                                    // every layout: never the VPinballX.ini file, which may live elsewhere

   bool operator==(const Declare&) const = default;
};

// Two Declares name different tables: another ROM, kind or machine. A hybrid table's ROM
// replaces its authored uuid within one table start, so this, not the epoch alone, is a switch.
inline bool NewIdentity(const Declare& was, const Declare& now)
{
   return was.romId != now.romId || was.tableKind != now.tableKind || was.machineUuid != now.machineUuid;
}

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

// --- Slice 2 memory reads -------

// Limits the daemon and the plugin both enforce. A subscription replaces the
// previous one wholesale; ReadDirect is one range per message.
inline constexpr size_t MAX_SUBSCRIBE_RANGES = 64;
inline constexpr size_t MAX_SUBSCRIBE_BYTES = 16 * 1024;
inline constexpr uint32_t MAX_READ_DIRECT_BYTES = 64 * 1024;
// ReadDirect reads each chunk of this size twice and reports those that disagreed.
inline constexpr uint32_t READ_CHUNK_BYTES = 256;

struct MemoryRange
{
   uint32_t address = 0;
   uint32_t length = 0;

   bool operator==(const MemoryRange&) const = default;
};

// Subscribe request, and its reply: the set the plugin will serve, echoed exactly.
struct Subscription
{
   std::vector<MemoryRange> ranges;

   bool operator==(const Subscription&) const = default;
};

struct MemoryBlock
{
   uint32_t address = 0;
   std::vector<uint8_t> bytes;      // its length is the range's length on the wire

   bool operator==(const MemoryBlock&) const = default;
};

// Poll reply. The request has an empty payload.
struct PollReply
{
   uint64_t frame = 0;              // rendered-frame counter, Pong's value; not the DMD frame id
   uint8_t playerRunning = 0;
   std::vector<MemoryBlock> blocks; // exactly the subscribed set, in subscription order

   bool operator==(const PollReply&) const = default;
};

struct ReadDirectRequest
{
   uint32_t address = 0;
   uint32_t length = 0;             // 1 to MAX_READ_DIRECT_BYTES

   bool operator==(const ReadDirectRequest&) const = default;
};

struct ReadDirectReply
{
   uint64_t frame = 0;
   uint32_t address = 0;
   std::vector<uint32_t> unstableOffsets; // multiples of READ_CHUNK_BYTES from address
   std::vector<uint8_t> bytes;            // may be shorter than requested

   bool operator==(const ReadDirectReply&) const = default;
};

// --- Slice 2 memory writes ---------------------------------------------------

// One Write is one PMPI_WRITE_MEMORY: its ops are applied in order, in one pass on
// the emulation thread. The daemon puts any NVRAM unlock inside the batch itself.
inline constexpr size_t MAX_WRITE_OPS = 16;

struct WriteOp
{
   uint32_t address = 0;
   std::vector<uint8_t> bytes;      // u16 length on the wire

   bool operator==(const WriteOp&) const = default;
};

struct WriteRequest
{
   std::vector<WriteOp> ops;        // 1 to MAX_WRITE_OPS

   bool operator==(const WriteRequest&) const = default;
};

struct WriteReply
{
   uint64_t frame = 0;
   uint16_t applied = 0;            // ops applied: count on success

   bool operator==(const WriteReply&) const = default;
};

// Informational: the protected window and its lock, as setNvRamParameters has them.
// The reply is empty, and the plugin adds no unlock of its own.
struct NvRamParameters
{
   uint32_t start = 0;
   uint32_t length = 0;
   uint32_t unlockAddress = 0;
   uint8_t unlockValue = 0;
   uint8_t lockValue = 0;

   bool operator==(const NvRamParameters&) const = default;
};

// --- Slice 4 overlay and status ---

inline constexpr uint8_t OVERLAY_OP_UPLOAD = 1;   // payload is the probe's 0x23 WriteDmdOverlay
inline constexpr uint8_t OVERLAY_OP_CONTROL = 2;  // payload is the probe's 0x27 control register
inline constexpr uint8_t OVERLAY_CONTROL_ENABLE = 1u << 0; // the other control bits are reserved

// The probe payload for the op is carried byte for byte: everything after op. The
// reply is empty.
struct OverlayRequest
{
   uint8_t op = 0;
   std::vector<uint8_t> payload;

   bool operator==(const OverlayRequest&) const = default;
};

// The 0x27 payload. duration_ms is the protocol's one big-endian field: the probe's
// byte order, kept because the payload is carried verbatim. 0 shows until hidden.
struct OverlayControl
{
   uint8_t control = 0;
   uint16_t durationMs = 0;

   bool operator==(const OverlayControl&) const = default;
};

inline constexpr size_t MAX_SUPPORT_TEXT_BYTES = 256;

enum SessionState : uint8_t
{
   SESSION_NONE = 0,       // no game in progress
   SESSION_TRACKING = 1,
   SESSION_NO_DATA = 2,    // a game should be scored and is not
   SESSION_HIDDEN = 4,     // the daemon's indicator preference is off; system_state is ignored
};

enum SystemState : uint8_t
{
   SYSTEM_OK = 0,
   SYSTEM_UNPAIRED = 1,
   SYSTEM_OFFLINE = 2,
   SYSTEM_DEGRADED = 3,
};

// Appended to Status from minor 2. The contract's own values.
enum AuthStatus : uint8_t
{
   AUTH_NOT_AUTHENTICATED = 0,
   AUTH_AUTHENTICATING = 1,
   AUTH_CHECKING_PAIRING = 2,
   AUTH_UNPAIRED = 3,
   AUTH_PAIRED = 4,
   AUTH_FAILED = 5,
};

// Status request. The reply is empty.
struct Status
{
   uint8_t sessionState = SESSION_NONE;
   uint8_t systemState = SYSTEM_OK;
   std::string supportText;    // UTF-8, at most MAX_SUPPORT_TEXT_BYTES, may be empty
   // 1.2 tail; a 1.0/1.1 daemon sends none.
   bool hasAuthStatus = false;
   uint8_t authStatus = AUTH_NOT_AUTHENTICATED;

   bool operator==(const Status&) const = default;
};

// --- Slice 5 authored tables (protocol 1.1) -------------------------------------

inline constexpr size_t MAX_GAME_OPS = 256;

enum GameOpKind : uint8_t
{
   GAME_START = 1,               // value: origin, 0 start button, 1 lobby
   GAME_END = 2,
   GAME_BALL = 3,                // value: ball
   GAME_ACTIVE_PLAYER = 4,       // value: player
   GAME_SCORE = 5,               // value: player; score, feature
   GAME_ADD_MODE = 6,            // mode
   GAME_ADD_MODE_EXPIRING = 7,   // seconds, mode
   GAME_MODE_COMPLETED = 8,      // mode
   GAME_REMOVE_MODE = 9,         // mode
   GAME_CLEAR_MODES = 10,
};

// One op of a commit. Only the fields its kind names are on the wire.
struct GameOp
{
   uint8_t kind = 0;
   uint8_t value = 0;
   int64_t score = 0;
   int32_t feature = 0;
   uint32_t seconds = 0;
   std::string mode;

   bool operator==(const GameOp&) const = default;
};

// Game request: one commit, in call order. The reply is empty. batch_id leads on the wire:
// counted from 1 per plugin load and kept across reconnects, so the daemon acknowledges a
// replayed batch without applying it twice.
struct GameBatch
{
   std::vector<GameOp> ops;
   uint32_t batchId = 0;

   bool operator==(const GameBatch&) const = default;
};

// --- Slice 6 read-backs (protocol 1.2) ----------------------------------------

// The contract's enum values; the daemon maps them by name.
enum TopScoresScope : uint8_t { SCOPE_MACHINE = 0, SCOPE_VARIANT = 1, SCOPE_GAME = 2 };
enum TopScoresPeriod : uint8_t
{
   PERIOD_ALL_TIME = 0,
   PERIOD_DAYS_14 = 1,
   PERIOD_DAYS_30 = 2,
   PERIOD_DAYS_90 = 3,
   PERIOD_DAYS_180 = 4,
   PERIOD_DAYS_365 = 5,
};
enum VpinFilter : uint8_t { VPIN_ANY = 0, VPIN_ONLY = 1, VPIN_REAL_ONLY = 2 };
enum TopScoresResult : uint8_t
{
   TOP_SCORES_OK = 0,
   TOP_SCORES_NOT_PAIRED = 1,
   TOP_SCORES_API_ERROR = 2,
   TOP_SCORES_FAILED = 3,
   TOP_SCORES_SUPERSEDED = 4,
};
enum TopScoreFlags : uint8_t
{
   SCORE_IS_VPIN = 1u << 0,
   SCORE_VERIFIED = 1u << 1,
   SCORE_NFC_VERIFIED = 1u << 2,
};

inline constexpr uint8_t MAX_TOP_SCORES = 20;
inline constexpr size_t MAX_SINCE_BYTES = 32;
inline constexpr size_t MAX_NAME_BYTES = 64;      // display_name, name, preferred_name
inline constexpr size_t MAX_INITIALS_BYTES = 8;
inline constexpr size_t MAX_CREATED_BYTES = 32;
inline constexpr size_t MAX_CLAIM_DEEPLINK_BYTES = 512;
inline constexpr uint8_t MAX_PLAYERS = 9;

// The reply is empty; the result comes later as its own TopScores.
struct TopScoresRequest
{
   uint32_t requestId = 0;
   uint8_t scope = SCOPE_MACHINE;
   uint8_t period = PERIOD_ALL_TIME;
   uint8_t vpinFilter = VPIN_ANY;
   uint8_t limit = 10;              // 1 to MAX_TOP_SCORES
   std::string since;               // "" or UTC ISO-8601, at most MAX_SINCE_BYTES

   bool operator==(const TopScoresRequest&) const = default;
};

struct TopScoreEntry
{
   uint16_t rank = 0;
   int64_t highScore = 0;
   std::string displayName;
   std::string initials;
   uint8_t flags = 0;               // TopScoreFlags
   std::string created;

   bool operator==(const TopScoreEntry&) const = default;
};

struct TopScores
{
   uint32_t requestId = 0;
   uint8_t result = TOP_SCORES_OK;
   uint32_t ageS = 0;               // seconds since the daemon fetched it
   std::vector<TopScoreEntry> entries; // empty unless result is ok

   bool operator==(const TopScores&) const = default;
};

// A slot is claimed exactly when playerId is not empty.
struct PlayerSlot
{
   uint8_t player = 0;              // 1 to MAX_PLAYERS
   std::string playerId;
   std::string preferredName;
   std::string name;
   std::string initials;
   std::string claimDeeplink;       // unclaimed slots only; sensitive

   bool operator==(const PlayerSlot&) const = default;
};

// The whole slot table, every time; a slot not listed is empty.
struct Players
{
   std::vector<PlayerSlot> slots;   // ascending by player, each at most once

   bool operator==(const Players&) const = default;
};

struct PlayerPicture
{
   uint8_t player = 0;
   std::string playerId;            // the claim the picture belongs to
   std::string path;                // absolute local JPEG path, "" for none; sensitive

   bool operator==(const PlayerPicture&) const = default;
};

std::vector<uint8_t> Encode(const Hello& v);
std::vector<uint8_t> Encode(const HelloAck& v);
std::vector<uint8_t> Encode(const FrameRequest& v);
std::vector<uint8_t> Encode(const FrameReply& v);
std::vector<uint8_t> Encode(const Pong& v);
std::vector<uint8_t> Encode(const Bye& v);
std::vector<uint8_t> Encode(const ErrorPayload& v);
std::vector<uint8_t> Encode(const Subscription& v);
std::vector<uint8_t> Encode(const PollReply& v);
std::vector<uint8_t> Encode(const ReadDirectRequest& v);
std::vector<uint8_t> Encode(const ReadDirectReply& v);
std::vector<uint8_t> Encode(const WriteRequest& v);
std::vector<uint8_t> Encode(const WriteReply& v);
std::vector<uint8_t> Encode(const NvRamParameters& v);
std::vector<uint8_t> Encode(const OverlayRequest& v);
std::vector<uint8_t> Encode(const OverlayControl& v);
std::vector<uint8_t> Encode(const Status& v);
std::vector<uint8_t> Encode(const GameBatch& v);
// These four refuse (empty) what their Decode refuses. When sensitive is given, it receives the
// spans of the claim deeplinks or the path.
std::vector<uint8_t> Encode(const TopScoresRequest& v);
std::vector<uint8_t> Encode(const TopScores& v);
std::vector<uint8_t> Encode(const Players& v, std::vector<SensitiveSpan>* sensitive = nullptr);
std::vector<uint8_t> Encode(const PlayerPicture& v, std::vector<SensitiveSpan>* sensitive = nullptr);

// The layout the peer negotiated: minor 0 stops after identify_format, 1 appends the slice 5
// fields. When sensitive is given, it receives the spans of the three path strings.
std::vector<uint8_t> Encode(const Declare& v, uint16_t peerMinor, std::vector<SensitiveSpan>* sensitive = nullptr);

// Every Decode requires each field it declares to be present and ignores any
// bytes trailing them. That is the compatibility rule: appending a field is a
// minor bump, so a 1.0 reader must not refuse a 1.1 payload it can still read.
bool Decode(const std::vector<uint8_t>& payload, Hello& out);
bool Decode(const std::vector<uint8_t>& payload, HelloAck& out);
// Reads the 1.1 fields when the payload carries them, and leaves them empty when it does not.
bool Decode(const std::vector<uint8_t>& payload, Declare& out);
bool Decode(const std::vector<uint8_t>& payload, FrameRequest& out);
bool Decode(const std::vector<uint8_t>& payload, FrameReply& out);
bool Decode(const std::vector<uint8_t>& payload, Pong& out);
bool Decode(const std::vector<uint8_t>& payload, Bye& out);
bool Decode(const std::vector<uint8_t>& payload, ErrorPayload& out);
bool Decode(const std::vector<uint8_t>& payload, Subscription& out);
bool Decode(const std::vector<uint8_t>& payload, PollReply& out);
bool Decode(const std::vector<uint8_t>& payload, ReadDirectRequest& out);
bool Decode(const std::vector<uint8_t>& payload, ReadDirectReply& out);
bool Decode(const std::vector<uint8_t>& payload, WriteRequest& out);
bool Decode(const std::vector<uint8_t>& payload, WriteReply& out);
bool Decode(const std::vector<uint8_t>& payload, NvRamParameters& out);
bool Decode(const std::vector<uint8_t>& payload, OverlayRequest& out);
bool Decode(const std::vector<uint8_t>& payload, OverlayControl& out);
// Also false when support_text is over MAX_SUPPORT_TEXT_BYTES. From peerMinor 2 the auth_status
// tail is read when present, and an unknown value is false; below it, trailing bytes are ignored.
bool Decode(const std::vector<uint8_t>& payload, Status& out, uint16_t peerMinor = PROTO_MINOR);
// False unless the count is 1 to MAX_GAME_OPS, every kind is known, and game_start is first
// and game_end last when present.
bool Decode(const std::vector<uint8_t>& payload, GameBatch& out);
// False for an enum out of range, a limit of 0 or over MAX_TOP_SCORES, an over-long string, more
// entries than MAX_TOP_SCORES or any with a result other than ok, or slots not 1 to 9 ascending.
bool Decode(const std::vector<uint8_t>& payload, TopScoresRequest& out);
bool Decode(const std::vector<uint8_t>& payload, TopScores& out);
bool Decode(const std::vector<uint8_t>& payload, Players& out, std::vector<SensitiveSpan>* sensitive = nullptr);
bool Decode(const std::vector<uint8_t>& payload, PlayerPicture& out, std::vector<SensitiveSpan>* sensitive = nullptr);

// The sensitive bytes of a daemon request, for a wire dump: the claim deeplinks of a Players and
// the path of a PlayerPicture. One of those that does not decode is sensitive whole. Offsets count
// from the first payload byte.
std::vector<SensitiveSpan> ReceivedSensitive(const Message& msg);

}
