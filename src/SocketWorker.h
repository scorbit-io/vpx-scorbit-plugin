// license:GPLv3+

#pragma once

// The plugin side of the Scorbit transport: one thread that owns one socket.
//
// The daemon listens on a user-scoped local socket and this connects to it,
// says Hello, Declares what the table is showing, and then answers the
// daemon's requests until either side hangs up. Nothing here touches the
// Visual Pinball plugin bus: posting to the bus from a worker is unsafe
// because ProcessAsyncCallbacks fires a timer before erasing it, so a nested
// pump can run a callback twice. Everything the worker needs about the session
// is published into an ISessionSource by the API thread and read back without
// ever making the API thread wait.
//
// The dependency on Visual Pinball is therefore in VpxSessionSource, not here,
// which is what lets the worker run against a scripted peer in the tests.

#include "Latch.h"
#include "MemoryBridge.h"
#include "OverlayState.h"
#include "StatusNotice.h"
#include "WireProtocol.h"
#include "WireDump.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace Scorbit
{

// The latest complete DMD frame, as the tap holds it.
struct FrameSnapshot
{
   // False when no conforming display source is selected, or when one is
   // selected but has not produced a frame yet. Both are steady states in
   // which no frame will be reported and the daemon must fail closed.
   bool hasSource = false;
   uint32_t frameId = 0;
   uint32_t generation = 0;
   uint16_t width = 0;
   uint16_t height = 0;
   uint8_t shades = 0;              // 4 or 16
   std::vector<uint8_t> pixels;     // width * height shade indices, row major
};

// What the plugin declares about itself, plus a counter the API thread bumps
// on a table start. The counter is not on the wire: it exists so a restart of
// the same table with the same display still re-sends Declare.
struct DeclareSnapshot
{
   Wire::Declare declare;
   uint64_t epoch = 0;
   // Not on the wire either: bumped whenever the overlay surface appears, goes or
   // moves to another source, which must re-Declare (design.md, slice 4) even when
   // nothing declared changes.
   uint64_t surfaceGeneration = 0;

   bool operator==(const DeclareSnapshot&) const = default;
};

// Answers to Ping.
struct SessionSnapshot
{
   uint64_t renderedFrame = 0;
   bool playerRunning = false;
};

// One committed batch of an authored table's game calls (slice 5), tagged with the Declare
// epoch it was committed under, so a batch from a table that has since changed is dropped.
struct GamePost
{
   uint64_t epoch = 0;
   Wire::GameBatch batch;
};

// Everything the worker reads about the running game. Every method must be
// safe to call from the worker thread.
class ISessionSource
{
public:
   virtual ~ISessionSource() = default;

   // The latest declaration the API thread posted, once; null when nothing new arrived.
   virtual std::unique_ptr<DeclareSnapshot> TakeDeclare() = 0;
   virtual void GetFrame(FrameSnapshot& out) = 0;
   virtual void GetSession(SessionSnapshot& out) = 0;
   // Appends every batch committed since the last call to out, in commit order.
   virtual void TakeGames(std::deque<GamePost>& out) { }
};

// Where Overlay requests go (slice 4). Every method is called on the worker thread and
// must never wait on the API thread.
class IOverlayTarget
{
public:
   virtual ~IOverlayTarget() = default;

   // The controller publishes a display the plugin can composite on.
   virtual bool OverlaySurface() = 0;
   // The 0x23 payload, verbatim.
   virtual Overlay::State::Upload OverlayUpload(const uint8_t* payload, size_t size) = 0;
   virtual void OverlayControl(bool enable, uint32_t durationMs) = 0;
   // Session end: hidden, with no bitmap and no timer.
   virtual void OverlayClear() = 0;
};

enum LogLevel
{
   LOG_LEVEL_DEBUG = 0,
   LOG_LEVEL_INFO = 1,
   LOG_LEVEL_WARN = 2,
   LOG_LEVEL_ERROR = 3,
};

struct SocketWorkerConfig
{
   // Empty means the per-platform rendezvous path.
   std::string socketPath;
   // Empty means vpx.token beside the socket.
   std::string tokenPath;
   std::string pluginVersion;
   std::string pluginApiCommit;
   // The table-script bindings are registered: Hello advertises authored_tables.
   bool authoredTables = false;
};

class SocketWorker final
{
public:
   using LogFn = std::function<void(int level, const std::string& message)>;

   // memory, when given, serves the slice 2 reads and makes Hello advertise
   // memory_read. overlay, when given, serves Overlay and advertises overlay; status,
   // when given, serves Status and carries the session state to the API thread. Each
   // must outlive the worker.
   SocketWorker(ISessionSource& source, SocketWorkerConfig config, LogFn log, MemoryBridge* memory = nullptr,
      IOverlayTarget* overlay = nullptr, StatusLatch* status = nullptr);
   ~SocketWorker();

   SocketWorker(const SocketWorker&) = delete;
   SocketWorker& operator=(const SocketWorker&) = delete;

   // Starts the worker thread. Safe to call once.
   void Start();
   // Signals the thread and joins it. Idempotent; the destructor calls it.
   void Stop();

   // Any thread. True between HelloAck and the end of that session.
   bool Connected() const { return m_connected; }

   // Where the daemon is expected to be listening, per platform.
   static std::string DefaultSocketPath();
   // vpx.token beside the given socket path.
   static std::string DefaultTokenPath(const std::string& socketPath);

private:
   using Clock = std::chrono::steady_clock;

   void Worker();
   void Log(int level, const std::string& message) const;
   // Logs once per distinct message until the next successful session, so a
   // daemon that is not running does not fill the log at one line per second.
   void LogOnce(int level, const std::string& message);

   bool ReadToken(std::string& token);
   bool Connect();
   enum class ConnectOutcome { Connected, Stopped, NoSuchPath, Refused, NotAccepting, Failed };
   struct ConnectResult
   {
      ConnectOutcome outcome;
      int error; // errno, or a WSA code on Windows; 0 when there is none
   };
   // Connects a nonblocking socket, waiting in stop-aware slices for one that is in progress.
   ConnectResult ConnectNonBlocking(uintptr_t socket, const void* addr, int addrLen);
   std::string DescribeConnectFailure(const ConnectResult& result) const;
   // False only when the endpoint path is definitely absent.
   bool PathExists() const;
   void Disconnect();
   bool Handshake();
   void Serve();
   bool MaybeDeclare();
   // Sends the batches committed under the Declare last sent, drops older ones, and leaves
   // newer ones for after their Declare. False closes the session.
   bool SendGames();
   // HelloAck offered authored tables at a minor that carries them.
   bool AuthoredAllowed() const;
   // Moves what the source committed into m_games, giving each batch the next id.
   void TakeSourceGames();
   // Takes a newly posted declaration into m_current, if there is one.
   void TakeDeclare();
   bool Dispatch(const Wire::Message& msg);
   bool AnswerFrame(const Wire::Message& msg);
   bool AnswerPing(const Wire::Message& msg);
   bool AnswerSubscribe(const Wire::Message& msg);
   bool AnswerPoll(const Wire::Message& msg);
   bool AnswerReadDirect(const Wire::Message& msg);
   bool AnswerWrite(const Wire::Message& msg);
   bool AnswerNvRam(const Wire::Message& msg);
   bool AnswerOverlay(const Wire::Message& msg);
   bool AnswerStatus(const Wire::Message& msg);
   // Hands the session state to the API thread, if anyone is listening.
   void PostStatus();
   void NoSession(const std::string& reason);
   // Sends the error for a bridge result other than Ok.
   bool SendMemoryError(const Wire::Message& msg, MemoryBridge::Result result);

   bool SendAll(const uint8_t* data, size_t size, int timeoutMs);
   // Not SendMessage: windows.h defines that as a macro expanding to SendMessageA
   // or SendMessageW, so the declaration and the definition textually disagree and
   // MSVC rejects the member. Renamed rather than #undef'd, because any later
   // include of windows.h would quietly put the macro back.
   // sensitive counts from the start of the payload.
   bool SendWireMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload,
      const std::vector<Wire::SensitiveSpan>& sensitive = { });
   bool SendError(uint16_t type, uint32_t seq, uint16_t code, const std::string& reason);
   bool SendIdlePing();
   // Sends a request and pumps the socket until its response arrives, serving
   // any request that overtakes it. False closes the session.
   bool Exchange(uint16_t type, const std::vector<uint8_t>& payload, int timeoutMs, Wire::Message& reply,
      const std::vector<Wire::SensitiveSpan>& sensitive = { });
   // -1 error, 0 timeout, 1 readable.
   int WaitReadable(int timeoutMs);
   bool ReadExact(uint8_t* dst, size_t size, int timeoutMs);
   bool ReadMessage(Wire::Message& out, int timeoutMs);
   void Backoff();

   ISessionSource& m_source;
   MemoryBridge* const m_memory;
   IOverlayTarget* const m_overlay;
   StatusLatch* const m_status;
   const SocketWorkerConfig m_config;
   const LogFn m_log;
   const uint64_t m_instanceId;

   WireDump m_wireDump;
   std::string m_socketPath;
   std::string m_tokenPath;

   std::atomic<bool> m_running { false };
   std::atomic<bool> m_connected { false };
   std::thread m_thread;

   // Session state, worker thread only.
#ifdef _WIN32
   uintptr_t m_socket = ~static_cast<uintptr_t>(0);
#else
   int m_socket = -1;
#endif
   uint32_t m_seq = 0;
   // The latest declaration taken from the latch, kept across connections so a new
   // one re-declares it after HelloAck, and the one this connection last sent.
   DeclareSnapshot m_current;
   DeclareSnapshot m_declared;
   bool m_haveDeclared = false;
   // What HelloAck offered this connection.
   uint16_t m_peerMinor = 0;
   uint32_t m_peerCapabilities = 0;
   bool m_loggedNoAuthored = false;      // once per connection
   // Batches taken from the source and not yet acknowledged, kept across connections.
   std::deque<GamePost> m_games;
   // The Declare epoch whose game_start the daemon acknowledged, 0 when no game is open. The
   // daemon keeps that game open across a reconnect to the same table, so its tail goes on as is.
   uint64_t m_gameOpenEpoch = 0;
   // Each batch's id is fixed when it is taken from the source, so a replay carries its own.
   uint32_t m_nextBatchId = 1;
   // Game outcomes already logged since the last Declare: error codes by bit, and bit 0 for
   // batches dropped because nothing authored was declared.
   uint32_t m_gameLogged = 0;
   bool m_loggedNvRam = false;           // NvRam is logged once per session
   Clock::time_point m_lastActivity { };
   std::string m_lastOnce;
   StatusRecord m_statusRecord;          // what was last posted to m_status
};

}
