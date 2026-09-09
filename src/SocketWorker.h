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
// is published into an ISessionSource by the API thread and read back under
// that implementation's own lock.
//
// The dependency on Visual Pinball is therefore in VpxSessionSource, not here,
// which is what lets the worker run against a scripted peer in the tests.

#include "WireProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

   bool operator==(const DeclareSnapshot&) const = default;
};

// Answers to Ping.
struct SessionSnapshot
{
   uint64_t renderedFrame = 0;
   bool playerRunning = false;
};

// Everything the worker reads about the running game. Every method must be
// safe to call from the worker thread.
class ISessionSource
{
public:
   virtual ~ISessionSource() = default;

   virtual void GetDeclare(DeclareSnapshot& out) = 0;
   virtual void GetFrame(FrameSnapshot& out) = 0;
   virtual void GetSession(SessionSnapshot& out) = 0;
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
};

class SocketWorker final
{
public:
   using LogFn = std::function<void(int level, const std::string& message)>;

   SocketWorker(ISessionSource& source, SocketWorkerConfig config, LogFn log);
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
   void Disconnect();
   bool Handshake();
   void Serve();
   bool MaybeDeclare();
   bool Dispatch(const Wire::Message& msg);
   bool AnswerFrame(const Wire::Message& msg);
   bool AnswerPing(const Wire::Message& msg);

   bool SendAll(const uint8_t* data, size_t size, int timeoutMs);
   bool SendMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload);
   bool SendError(uint16_t type, uint32_t seq, uint16_t code, const std::string& reason);
   bool SendIdlePing();
   // Sends a request and pumps the socket until its response arrives, serving
   // any request that overtakes it. False closes the session.
   bool Exchange(uint16_t type, const std::vector<uint8_t>& payload, int timeoutMs, Wire::Message& reply);
   // -1 error, 0 timeout, 1 readable.
   int WaitReadable(int timeoutMs);
   bool ReadExact(uint8_t* dst, size_t size, int timeoutMs);
   bool ReadMessage(Wire::Message& out, int timeoutMs);
   void Backoff();

   ISessionSource& m_source;
   const SocketWorkerConfig m_config;
   const LogFn m_log;
   const uint64_t m_instanceId;

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
   DeclareSnapshot m_declared;
   bool m_haveDeclared = false;
   Clock::time_point m_lastActivity { };
   std::string m_lastOnce;
};

}
