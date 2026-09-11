// license:GPLv3+

#include "SocketWorker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace std::string_literals;

#ifdef _WIN32
   #define WIN32_LEAN_AND_MEAN
   #include <winsock2.h>
   #include <afunix.h>
   #include <windows.h>
   using socket_t = SOCKET;
   #define SOCKET_ERRNO WSAGetLastError()
#else
   #include <cerrno>
   #include <fcntl.h>
   #include <poll.h>
   #include <sys/socket.h>
   #include <sys/stat.h>
   #include <sys/un.h>
   #include <unistd.h>
   #include <pthread.h>
   using socket_t = int;
   #define SOCKET_ERRNO errno
   #define INVALID_SOCKET (-1)
#endif

namespace Scorbit
{

namespace
{

// The daemon is the rendezvous, so a plugin that starts first simply retries.
constexpr int RECONNECT_INTERVAL_MS = 1000;
// Short enough that unload joins the thread promptly on every platform,
// including Windows where there is no self pipe to wake a poll early.
constexpr int POLL_SLICE_MS = 100;
constexpr int HELLO_TIMEOUT_MS = 1000;
constexpr int DECLARE_TIMEOUT_MS = 1000;
constexpr int PONG_TIMEOUT_MS = 2000;
constexpr int SEND_TIMEOUT_MS = 1000;
// Reading the rest of a message whose length header has already arrived.
constexpr int BODY_TIMEOUT_MS = 1000;
// The daemon polls; this only exists so a half open socket is noticed.
constexpr auto IDLE_PING_AFTER = std::chrono::milliseconds(500);

constexpr size_t TOKEN_CHARS = 64;

void NameThisThread(const char* name)
{
#if defined(_WIN32)
   wchar_t wname[64];
   if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64) > 0)
      SetThreadDescription(GetCurrentThread(), wname);
#elif defined(__APPLE__)
   pthread_setname_np(name);
#elif defined(__linux__) || defined(__ANDROID__)
   pthread_setname_np(pthread_self(), name);
#else
   (void)name;
#endif
}

bool IsHex(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string Trim(const std::string& s)
{
   size_t b = 0, e = s.size();
   while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
      b++;
   while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
      e--;
   return s.substr(b, e - b);
}

std::string ParentOf(const std::string& path)
{
#ifdef _WIN32
   const size_t cut = path.find_last_of("\\/");
#else
   const size_t cut = path.find_last_of('/');
#endif
   return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

int MillisUntil(std::chrono::steady_clock::time_point deadline)
{
   const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
   if (left <= 0)
      return 0;
   return left > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(left);
}

}

SocketWorker::SocketWorker(ISessionSource& source, SocketWorkerConfig config, LogFn log)
   : m_source(source)
   , m_config(std::move(config))
   , m_log(std::move(log))
   , m_instanceId([]
        {
           std::random_device rd;
           std::mt19937_64 gen(((static_cast<uint64_t>(rd()) << 32) ^ rd()));
           return gen();
        }())
{
   m_socketPath = m_config.socketPath.empty() ? DefaultSocketPath() : m_config.socketPath;
   m_tokenPath = m_config.tokenPath.empty() ? DefaultTokenPath(m_socketPath) : m_config.tokenPath;
}

SocketWorker::~SocketWorker()
{
   Stop();
}

std::string SocketWorker::DefaultSocketPath()
{
#if defined(_WIN32)
   char buf[MAX_PATH] = { };
   DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
   if (n == 0 || n >= MAX_PATH)
      return std::string();
   return std::string(buf) + "\\Scorbit\\vpx.sock";
#elif defined(__APPLE__)
   char buf[1024] = { };
   const size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf));
   if (n == 0 || n > sizeof(buf))
      return std::string("/tmp/scorbit/vpx.sock");
   std::string dir(buf);
   if (!dir.empty() && dir.back() == '/')
      dir.pop_back();
   return dir + "/scorbit/vpx.sock";
#else
   if (const char* xdg = getenv("XDG_RUNTIME_DIR"); xdg != nullptr && xdg[0] != '\0')
   {
      std::string dir(xdg);
      if (!dir.empty() && dir.back() == '/')
         dir.pop_back();
      return dir + "/scorbit/vpx.sock";
   }
   return "/run/user/" + std::to_string(static_cast<unsigned>(getuid())) + "/scorbit/vpx.sock";
#endif
}

std::string SocketWorker::DefaultTokenPath(const std::string& socketPath)
{
   const std::string dir = ParentOf(socketPath);
   if (dir.empty())
      return "vpx.token";
#ifdef _WIN32
   return dir + "\\vpx.token";
#else
   return dir + "/vpx.token";
#endif
}

void SocketWorker::Start()
{
   if (m_running.exchange(true))
      return;
#ifdef _WIN32
   WSADATA wsa { };
   if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
   {
      Log(LOG_LEVEL_ERROR, "Socket: WSAStartup failed, the transport is disabled");
      m_running = false;
      return;
   }
#endif
   if (m_socketPath.empty())
   {
      Log(LOG_LEVEL_ERROR, "Socket: no rendezvous path for this platform, the transport is disabled");
      m_running = false;
      return;
   }
   Log(LOG_LEVEL_INFO, "Socket: endpoint " + m_socketPath + ", token " + m_tokenPath);
   m_thread = std::thread(&SocketWorker::Worker, this);
}

void SocketWorker::Stop()
{
   const bool wasRunning = m_running.exchange(false);
   if (m_thread.joinable())
      m_thread.join();
#ifdef _WIN32
   if (wasRunning)
      WSACleanup();
#else
   (void)wasRunning;
#endif
}

void SocketWorker::Log(int level, const std::string& message) const
{
   if (m_log)
      m_log(level, message);
}

void SocketWorker::LogOnce(int level, const std::string& message)
{
   if (m_lastOnce == message)
      return;
   m_lastOnce = message;
   Log(level, message);
}

void SocketWorker::Worker()
{
   NameThisThread("Scorbit.Socket");

   // Opened here rather than in Start() so the file is owned end to end by the thread
   // that writes it. Logging goes through Log(), which queues to the API thread.
   std::string dumpError;
   m_wireDump.Open(dumpError);
   if (!dumpError.empty())
      Log(LOG_LEVEL_ERROR, dumpError);
   else if (m_wireDump.Enabled())
      Log(LOG_LEVEL_INFO, "Socket: recording wire frames (SCORBIT_WIRE_DUMP is set)");

   while (m_running)
   {
      if (!Connect())
      {
         Backoff();
         continue;
      }
      if (Handshake())
         Serve();
      Disconnect();
      if (m_running)
         Backoff();
   }

   m_wireDump.Close();
}

void SocketWorker::Backoff()
{
   // Slice 1 retries at a flat one hertz; exponential backoff is slice 5.
   for (int waited = 0; waited < RECONNECT_INTERVAL_MS && m_running; waited += POLL_SLICE_MS)
      std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLICE_MS));
}

bool SocketWorker::ReadToken(std::string& token)
{
   // Read immediately before every connection attempt: the daemon rotates the
   // token when it rebinds, and a cached one would fail the next handshake.
   FILE* f = fopen(m_tokenPath.c_str(), "rb");
   if (f == nullptr)
   {
      LogOnce(LOG_LEVEL_INFO, "Socket: no token at " + m_tokenPath + ", waiting for the daemon");
      return false;
   }
   char buf[256] = { };
   const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);

   token = Trim(std::string(buf, n));
   if (token.size() != TOKEN_CHARS)
   {
      LogOnce(LOG_LEVEL_WARN, "Socket: token at " + m_tokenPath + " is " + std::to_string(token.size())
         + " characters, expected " + std::to_string(TOKEN_CHARS));
      return false;
   }
   for (const char c : token)
   {
      if (!IsHex(c))
      {
         LogOnce(LOG_LEVEL_WARN, "Socket: token at " + m_tokenPath + " is not hexadecimal");
         return false;
      }
   }
   return true;
}

bool SocketWorker::Connect()
{
   sockaddr_un addr { };
   addr.sun_family = AF_UNIX;
   if (m_socketPath.size() >= sizeof(addr.sun_path))
   {
      LogOnce(LOG_LEVEL_ERROR, "Socket: endpoint path is longer than " + std::to_string(sizeof(addr.sun_path) - 1)
         + " characters: " + m_socketPath);
      return false;
   }
   std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size());

   const socket_t s = socket(AF_UNIX, SOCK_STREAM, 0);
   if (s == INVALID_SOCKET)
   {
      LogOnce(LOG_LEVEL_ERROR, "Socket: cannot create a socket (" + std::to_string(SOCKET_ERRNO) + ')');
      return false;
   }

   if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
   {
      LogOnce(LOG_LEVEL_INFO, "Socket: no daemon at " + m_socketPath + ", retrying every second");
#ifdef _WIN32
      closesocket(s);
#else
      close(s);
#endif
      return false;
   }

#ifdef _WIN32
   u_long nonblocking = 1;
   ioctlsocket(s, FIONBIO, &nonblocking);
#else
   const int flags = fcntl(s, F_GETFL, 0);
   if (flags >= 0)
      fcntl(s, F_SETFL, flags | O_NONBLOCK);
   #ifdef SO_NOSIGPIPE
   // macOS has no MSG_NOSIGNAL; without this a daemon that hangs up mid write
   // would take the whole VPX process down with SIGPIPE.
   const int on = 1;
   setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
   #endif
#endif

#ifdef _WIN32
   m_socket = static_cast<uintptr_t>(s);
#else
   m_socket = s;
#endif
   m_seq = 0;
   m_haveDeclared = false;
   m_declared = { };
   return true;
}

void SocketWorker::Disconnect()
{
#ifdef _WIN32
   if (m_socket != ~static_cast<uintptr_t>(0))
   {
      closesocket(static_cast<socket_t>(m_socket));
      m_socket = ~static_cast<uintptr_t>(0);
   }
#else
   if (m_socket >= 0)
   {
      close(m_socket);
      m_socket = -1;
   }
#endif
   if (m_connected.exchange(false))
      Log(LOG_LEVEL_INFO, "Socket: session ended");
}

int SocketWorker::WaitReadable(int timeoutMs)
{
   const socket_t s = static_cast<socket_t>(m_socket);
#ifdef _WIN32
   WSAPOLLFD pfd { };
   pfd.fd = s;
   pfd.events = POLLRDNORM;
   const int rc = WSAPoll(&pfd, 1, timeoutMs);
#else
   pollfd pfd { };
   pfd.fd = s;
   pfd.events = POLLIN;
   int rc;
   do
      rc = poll(&pfd, 1, timeoutMs);
   while (rc < 0 && errno == EINTR);
#endif
   if (rc < 0)
      return -1;
   if (rc == 0)
      return 0;
   if ((pfd.revents & (POLLERR | POLLNVAL)) != 0)
      return -1;
   return 1;
}

bool SocketWorker::SendAll(const uint8_t* data, size_t size, int timeoutMs)
{
   const socket_t s = static_cast<socket_t>(m_socket);
   const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   size_t sent = 0;
   // Deliberately does not give up when the worker is stopping: the last thing
   // a session sends is Bye, after m_running is already false. The deadline is
   // what bounds this loop.
   while (sent < size)
   {
#ifdef _WIN32
      const int n = send(s, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
      const bool wouldBlock = n < 0 && (WSAGetLastError() == WSAEWOULDBLOCK);
#elif defined(MSG_NOSIGNAL)
      const ssize_t n = send(s, data + sent, size - sent, MSG_NOSIGNAL);
      const bool wouldBlock = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#else
      const ssize_t n = send(s, data + sent, size - sent, 0);
      const bool wouldBlock = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
      if (n > 0)
      {
         sent += static_cast<size_t>(n);
         continue;
      }
      if (!wouldBlock)
         return false;

      const int left = MillisUntil(deadline);
      if (left == 0)
         return false;
#ifdef _WIN32
      WSAPOLLFD pfd { };
      pfd.fd = s;
      pfd.events = POLLWRNORM;
      if (WSAPoll(&pfd, 1, left < POLL_SLICE_MS ? left : POLL_SLICE_MS) < 0)
         return false;
#else
      pollfd pfd { };
      pfd.fd = s;
      pfd.events = POLLOUT;
      if (poll(&pfd, 1, left < POLL_SLICE_MS ? left : POLL_SLICE_MS) < 0 && errno != EINTR)
         return false;
#endif
   }
   return true;
}

bool SocketWorker::ReadExact(uint8_t* dst, size_t size, int timeoutMs)
{
   const socket_t s = static_cast<socket_t>(m_socket);
   const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   size_t got = 0;
   while (got < size)
   {
      if (!m_running)
         return false;
#ifdef _WIN32
      const int n = recv(s, reinterpret_cast<char*>(dst + got), static_cast<int>(size - got), 0);
      const bool wouldBlock = n < 0 && (WSAGetLastError() == WSAEWOULDBLOCK);
#else
      const ssize_t n = recv(s, dst + got, size - got, 0);
      const bool wouldBlock = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
      if (n == 0)
         return false; // peer closed
      if (n > 0)
      {
         got += static_cast<size_t>(n);
         continue;
      }
      if (!wouldBlock)
         return false;

      const int left = MillisUntil(deadline);
      if (left == 0)
         return false;
      if (WaitReadable(left < POLL_SLICE_MS ? left : POLL_SLICE_MS) < 0)
         return false;
   }
   return true;
}

bool SocketWorker::ReadMessage(Wire::Message& out, int timeoutMs)
{
   uint8_t lengthBytes[Wire::LENGTH_BYTES];
   if (!ReadExact(lengthBytes, sizeof(lengthBytes), timeoutMs))
      return false;

   Wire::Reader lr(lengthBytes, sizeof(lengthBytes));
   uint32_t length = 0;
   lr.U32(length);

   if (length < Wire::HEADER_BYTES || length > Wire::MAX_MESSAGE_BYTES)
   {
      // The stream is unusable from here: the header this length belongs to has
      // not been read, so there is nothing to echo. Answer with a bare error and
      // close, which is what the daemon does with the mirror case.
      Log(LOG_LEVEL_ERROR, "Socket: message length " + std::to_string(length) + " is out of bounds, closing");
      SendError(0, 0, Wire::ERR_MALFORMED, "message length out of bounds");
      return false;
   }

   std::vector<uint8_t> body(length);
   if (!ReadExact(body.data(), body.size(), BODY_TIMEOUT_MS))
   {
      Log(LOG_LEVEL_ERROR, "Socket: message body of " + std::to_string(length) + " bytes never arrived, closing");
      return false;
   }
   m_wireDump.Received(lengthBytes, sizeof(lengthBytes), body.data(), body.size());

   if (!Wire::DecodeBody(body.data(), body.size(), out))
   {
      Log(LOG_LEVEL_ERROR, "Socket: message shorter than a header, closing");
      SendError(0, 0, Wire::ERR_MALFORMED, "message shorter than a header");
      return false;
   }
   return true;
}

bool SocketWorker::SendMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload)
{
   const std::vector<uint8_t> bytes = Wire::EncodeMessage(type, flags, seq, payload);
   if (bytes.empty())
   {
      Log(LOG_LEVEL_ERROR, "Socket: refusing to send an oversized "s + Wire::TypeName(type) + " message");
      return false;
   }
   m_wireDump.Sent(bytes.data(), bytes.size());
   return SendAll(bytes.data(), bytes.size(), SEND_TIMEOUT_MS);
}

bool SocketWorker::SendError(uint16_t type, uint32_t seq, uint16_t code, const std::string& reason)
{
   Wire::ErrorPayload e;
   e.code = code;
   e.reason = reason;
   return SendMessage(type, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, seq, Wire::Encode(e));
}

bool SocketWorker::Exchange(uint16_t type, const std::vector<uint8_t>& payload, int timeoutMs, Wire::Message& reply)
{
   const uint32_t seq = ++m_seq;
   if (!SendMessage(type, 0, seq, payload))
   {
      Log(LOG_LEVEL_WARN, "Socket: sending "s + Wire::TypeName(type) + " failed");
      return false;
   }

   const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   while (m_running)
   {
      const int left = MillisUntil(deadline);
      if (left == 0)
      {
         Log(LOG_LEVEL_WARN, "Socket: no answer to "s + Wire::TypeName(type) + " within "
            + std::to_string(timeoutMs) + " ms, closing");
         return false;
      }
      const int rc = WaitReadable(left < POLL_SLICE_MS ? left : POLL_SLICE_MS);
      if (rc < 0)
         return false;
      if (rc == 0)
         continue;

      Wire::Message msg;
      if (!ReadMessage(msg, left))
         return false;
      m_lastActivity = Clock::now();

      if (msg.header.IsResponse())
      {
         if (msg.header.seq == seq)
         {
            reply = std::move(msg);
            return true;
         }
         // A response to a request that already timed out. Nothing to do with
         // it, and dropping it keeps the stream in step.
         Log(LOG_LEVEL_DEBUG, "Socket: dropped a stale response, seq " + std::to_string(msg.header.seq));
         continue;
      }
      if (!Dispatch(msg))
         return false;
   }
   return false;
}

bool SocketWorker::Handshake()
{
   std::string token;
   if (!ReadToken(token))
      return false;

   Wire::Hello hello;
   hello.protoMajor = Wire::PROTO_MAJOR;
   hello.protoMinor = Wire::PROTO_MINOR;
   hello.pluginVersion = m_config.pluginVersion;
   hello.pluginApiCommit = m_config.pluginApiCommit;
   hello.token = token;
   hello.instanceId = m_instanceId;
   hello.capabilities = Wire::CAP_IDENTIFY_FRAMES;

   // Hello's vpx_revision is the build the plugin API headers were pinned to.
   // The plugin API exposes no runtime version, so this is what the plugin
   // knows rather than what is actually running; the daemon logs it as such.
   DeclareSnapshot declared;
   m_source.GetDeclare(declared);
   hello.vpxRevision = declared.declare.vpxRevision;

   Wire::Message reply;
   if (!Exchange(Wire::TYPE_HELLO, Wire::Encode(hello), HELLO_TIMEOUT_MS, reply))
      return false;

   if (reply.header.IsError())
   {
      Wire::ErrorPayload e;
      if (!Wire::Decode(reply.payload, e))
      {
         Log(LOG_LEVEL_ERROR, "Socket: Hello was refused with an unreadable error payload");
         return false;
      }
      // version_mismatch, bad_token and busy are all states an operator has to
      // fix, so they are reported once at error level and the session stops.
      // There is no plugin side amber state to raise yet; when the Status path
      // lands in slice 4 this is where it is set.
      LogOnce(LOG_LEVEL_ERROR, "Socket: daemon refused Hello: "s + Wire::ErrorName(e.code) + " - " + e.reason);
      return false;
   }

   // A non error response may echo the request type or carry HelloAck's own;
   // both are accepted so a stricter or looser daemon still interoperates.
   if (reply.header.type != Wire::TYPE_HELLO_ACK && reply.header.type != Wire::TYPE_HELLO)
   {
      Log(LOG_LEVEL_ERROR, "Socket: expected HelloAck, got "s + Wire::TypeName(reply.header.type));
      return false;
   }

   Wire::HelloAck ack;
   if (!Wire::Decode(reply.payload, ack))
   {
      Log(LOG_LEVEL_ERROR, "Socket: HelloAck payload is malformed, closing");
      return false;
   }
   if (ack.protoMajor != Wire::PROTO_MAJOR)
   {
      LogOnce(LOG_LEVEL_ERROR, "Socket: daemon speaks protocol " + std::to_string(ack.protoMajor) + '.'
         + std::to_string(ack.protoMinor) + ", this plugin speaks " + std::to_string(Wire::PROTO_MAJOR) + '.'
         + std::to_string(Wire::PROTO_MINOR));
      return false;
   }

   m_connected = true;
   m_lastOnce.clear();
   Log(LOG_LEVEL_INFO, "Socket: connected to daemon " + ack.daemonVersion + " (protocol "
      + std::to_string(ack.protoMajor) + '.' + std::to_string(ack.protoMinor)
      + ", session " + std::to_string(ack.sessionId) + ')');
   return true;
}

bool SocketWorker::MaybeDeclare()
{
   DeclareSnapshot now;
   m_source.GetDeclare(now);
   if (m_haveDeclared && now == m_declared)
      return true;

   Wire::Message reply;
   if (!Exchange(Wire::TYPE_DECLARE, Wire::Encode(now.declare), DECLARE_TIMEOUT_MS, reply))
      return false;
   if (reply.header.IsError())
   {
      Wire::ErrorPayload e;
      Wire::Decode(reply.payload, e);
      Log(LOG_LEVEL_ERROR, "Socket: daemon refused Declare: "s + Wire::ErrorName(e.code) + " - " + e.reason);
      return false;
   }

   m_declared = now;
   m_haveDeclared = true;
   const Wire::Declare& d = now.declare;
   Log(LOG_LEVEL_INFO, "Socket: declared romId=" + (d.romId.empty() ? "(none)" : d.romId)
      + " display=" + std::to_string(d.width) + 'x' + std::to_string(d.height)
      + '/' + std::to_string(d.shades) + " shades"
      + " generation=" + std::to_string(d.sourceGeneration)
      + " format=" + (d.identifyFormat.empty() ? "(none)" : d.identifyFormat));
   return true;
}

bool SocketWorker::SendIdlePing()
{
   Wire::Message reply;
   if (!Exchange(Wire::TYPE_PING, { }, PONG_TIMEOUT_MS, reply))
      return false;
   return true;
}

void SocketWorker::Serve()
{
   m_lastActivity = Clock::now();

   // A request can be in flight when unload arrives, and Exchange gives up as
   // soon as the worker is stopping. That is a healthy session left on purpose,
   // so it still earns a Bye; only a genuine failure with the worker still
   // running means the socket is not worth writing to.
   bool alive = true;
   while (m_running)
   {
      if (!MaybeDeclare())
      {
         alive = !m_running;
         break;
      }

      const int rc = WaitReadable(POLL_SLICE_MS);
      if (rc < 0)
      {
         Log(LOG_LEVEL_WARN, "Socket: poll failed, closing");
         alive = false;
         break;
      }
      if (rc == 0)
      {
         if (Clock::now() - m_lastActivity > IDLE_PING_AFTER && !SendIdlePing())
         {
            alive = !m_running;
            break;
         }
         continue;
      }

      Wire::Message msg;
      if (!ReadMessage(msg, BODY_TIMEOUT_MS))
      {
         alive = !m_running;
         break;
      }
      m_lastActivity = Clock::now();

      if (msg.header.IsResponse())
      {
         Log(LOG_LEVEL_DEBUG, "Socket: dropped an unsolicited response, seq " + std::to_string(msg.header.seq));
         continue;
      }
      if (!Dispatch(msg))
      {
         alive = false;
         break;
      }
   }

   if (!alive)
      return;

   // Stopping on purpose: tell the daemon rather than letting it discover a
   // closed socket. Bye expects no response.
   SendMessage(Wire::TYPE_BYE, 0, ++m_seq, Wire::Encode(Wire::Bye { "plugin unloading" }));
   Log(LOG_LEVEL_INFO, "Socket: sent Bye");
}

bool SocketWorker::Dispatch(const Wire::Message& msg)
{
   switch (msg.header.type)
   {
   case Wire::TYPE_FRAME:
      return AnswerFrame(msg);
   case Wire::TYPE_PING:
      return AnswerPing(msg);
   case Wire::TYPE_BYE:
   {
      Wire::Bye bye;
      Wire::Decode(msg.payload, bye);
      Log(LOG_LEVEL_INFO, "Socket: daemon said Bye" + (bye.reason.empty() ? std::string() : ": " + bye.reason));
      return false;
   }
   default:
      // Everything else, including the types reserved for memory, overlay and
      // status, is refused explicitly so the daemon learns at once what this
      // build serves instead of waiting out a timeout.
      Log(LOG_LEVEL_DEBUG, "Socket: refusing "s + Wire::TypeName(msg.header.type) + " request");
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_UNSUPPORTED_TYPE,
         std::string(Wire::TypeName(msg.header.type)) + " is not served by this plugin");
   }
}

bool SocketWorker::AnswerFrame(const Wire::Message& msg)
{
   Wire::FrameRequest req;
   if (!Wire::Decode(msg.payload, req))
   {
      Log(LOG_LEVEL_ERROR, "Socket: malformed Frame request, closing");
      SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "Frame request payload");
      return false;
   }

   FrameSnapshot snapshot;
   m_source.GetFrame(snapshot);

   Wire::FrameReply out;
   out.sourceGeneration = snapshot.generation;
   if (snapshot.hasSource)
   {
      out.frameId = snapshot.frameId;
      out.width = snapshot.width;
      out.height = snapshot.height;
      out.shades = snapshot.shades;
      // The reserved since_frame_id says the daemon holds no frame, so nothing
      // it could send in since_generation makes this reply an unchanged one.
      const bool holdsNothing = req.sinceFrameId == Wire::SINCE_FRAME_NONE;
      const bool unchanged = !holdsNothing
         && snapshot.frameId == req.sinceFrameId
         && snapshot.generation == req.sinceGeneration;
      if (!unchanged)
      {
         out.hasPixels = 1;
         out.pixels = std::move(snapshot.pixels);
      }
   }

   std::vector<uint8_t> payload = Wire::Encode(out);
   if (payload.empty())
   {
      // Only reachable if a display is larger than the message bound allows.
      // The session survives; the daemon simply gets no pixels for it.
      LogOnce(LOG_LEVEL_ERROR, "Socket: " + std::to_string(out.width) + 'x' + std::to_string(out.height)
         + " does not fit the one mebibyte message bound");
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "frame exceeds the message bound");
   }
   return SendMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, payload);
}

bool SocketWorker::AnswerPing(const Wire::Message& msg)
{
   // Ping carries nothing today. Trailing bytes from a newer daemon are
   // ignored rather than refused, so a minor bump does not break the session.
   SessionSnapshot session;
   m_source.GetSession(session);

   Wire::Pong pong;
   pong.renderedFrame = session.renderedFrame;
   pong.playerRunning = session.playerRunning ? 1 : 0;
   pong.writesSupported = 0; // memory writes arrive in slice 2
   return SendMessage(Wire::TYPE_PONG, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(pong));
}

}
