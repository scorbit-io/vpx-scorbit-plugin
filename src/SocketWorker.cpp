// license:GPLv3+

#include "SocketWorker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <system_error>

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
// A nonblocking connect still in progress after this is treated as a daemon not accepting.
constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int DECLARE_TIMEOUT_MS = 1000;
constexpr int GAME_TIMEOUT_MS = 1000;
constexpr int PONG_TIMEOUT_MS = 2000;
constexpr int SEND_TIMEOUT_MS = 1000;
// Reading the rest of a message whose length header has already arrived.
constexpr int BODY_TIMEOUT_MS = 1000;
// How long a memory request waits for the API thread. The daemon allows 100 ms
// per request, so answering busy inside this leaves it time to ask again.
constexpr int MEMORY_BUDGET_MS = 50;
// The wait for VPX's thread to take a Write; an untaken write answers busy with nothing
// applied. Once taken, it is waited on to its answer, which pinmame #634 bounds at 250 ms.
constexpr int WRITE_BUDGET_MS = 40;
// The daemon polls; this only exists so a half open socket is noticed.
constexpr auto IDLE_PING_AFTER = std::chrono::milliseconds(500);

constexpr size_t TOKEN_CHARS = 64;

#ifdef _WIN32
// Endpoint paths are UTF-8 on Windows: sun_path is a UTF-8 byte path there, and the
// daemon resolves its own path that way (SB-4609). The environment and the file APIs
// are UTF-16, so every crossing is explicit and strict — a path that is not valid
// UTF-8 is refused rather than silently transcoded, which is what the daemon does.
std::string ToUtf8(const wchar_t* w, int wlen)
{
   const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wlen,
                                     nullptr, 0, nullptr, nullptr);
   if (n <= 0)
      return std::string();
   std::string out(static_cast<size_t>(n), '\0');
   if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, wlen, out.data(), n,
                           nullptr, nullptr) != n)
      return std::string();
   return out;
}

std::wstring ToWide(const std::string& utf8)
{
   if (utf8.empty())
      return std::wstring();
   const int len = static_cast<int>(utf8.size());
   const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, nullptr, 0);
   if (n <= 0)
      return std::wstring();
   std::wstring out(static_cast<size_t>(n), L'\0');
   if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), len, out.data(), n) != n)
      return std::wstring();
   return out;
}
#endif

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

SocketWorker::SocketWorker(ISessionSource& source, SocketWorkerConfig config, LogFn log, MemoryBridge* memory,
   IOverlayTarget* overlay, StatusLatch* status)
   : m_source(source)
   , m_memory(memory)
   , m_overlay(overlay)
   , m_status(status)
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
   m_statusRecord.since = Clock::now();
}

SocketWorker::~SocketWorker()
{
   Stop();
}

std::string SocketWorker::DefaultSocketPath()
{
#if defined(_WIN32)
   // Read wide and convert to UTF-8. Narrow gives the ANSI code page, so any profile
   // name with a non-ASCII character built a different byte path from the one the
   // daemon bound and never connected (SB-5007). Fails closed either way.
   wchar_t wbuf[MAX_PATH] = { };
   const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", wbuf, MAX_PATH);
   if (n == 0 || n >= MAX_PATH)
      return std::string();
   const std::string base = ToUtf8(wbuf, static_cast<int>(n));
   if (base.empty())
      return std::string();
   return base + "\\Scorbit\\vpx.sock";
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
         // No daemon to blame any more, so a refusal reason no longer applies.
         NoSession(std::string());
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
   char buf[256] = { };
   size_t n = 0;
#ifdef _WIN32
   // Opened wide: m_tokenPath is UTF-8, and the narrow CRT would read it as the ANSI
   // code page — missing the file for exactly the non-ASCII profile names the wide
   // path resolution above exists to fix.
   //
   // FILE_SHARE_DELETE is not incidental. The daemon replaces the token by writing a
   // .tmp and calling MoveFileExW(MOVEFILE_REPLACE_EXISTING); the CRT's fopen omits
   // that share, so a daemon rebinding inside this open would fail its own Initialize
   // with a sharing violation. POSIX rename over an open file has no such problem.
   const std::wstring wpath = ToWide(m_tokenPath);
   const HANDLE h = wpath.empty()
      ? INVALID_HANDLE_VALUE
      : CreateFileW(wpath.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (h == INVALID_HANDLE_VALUE)
   {
      LogOnce(LOG_LEVEL_INFO, "Socket: no token at " + m_tokenPath + ", waiting for the daemon");
      return false;
   }
   DWORD got = 0;
   if (ReadFile(h, buf, static_cast<DWORD>(sizeof(buf) - 1), &got, nullptr))
      n = got;
   CloseHandle(h);
#else
   FILE* f = fopen(m_tokenPath.c_str(), "rb");
   if (f == nullptr)
   {
      LogOnce(LOG_LEVEL_INFO, "Socket: no token at " + m_tokenPath + ", waiting for the daemon");
      return false;
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
#endif

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

   // Nonblocking before connect: on Linux a blocking connect to a listener whose
   // backlog is full waits until it accepts, which a stalled daemon never does,
   // and Stop() would then never join this thread (SB-5083).
#ifdef _WIN32
   u_long nonblocking = 1;
   const bool nonblockingSet = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
   const int flags = fcntl(s, F_GETFL, 0);
   const bool nonblockingSet = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
   if (!nonblockingSet)
   {
      const int error = SOCKET_ERRNO;
#ifdef _WIN32
      closesocket(s);
#else
      close(s);
#endif
      LogOnce(LOG_LEVEL_ERROR, "Socket: cannot make the socket nonblocking (" + std::to_string(error) + ')');
      return false;
   }

   const ConnectResult result = ConnectNonBlocking(static_cast<uintptr_t>(s), &addr, static_cast<int>(sizeof(addr)));
   if (result.outcome != ConnectOutcome::Connected)
   {
#ifdef _WIN32
      closesocket(s);
#else
      close(s);
#endif
      if (result.outcome != ConnectOutcome::Stopped)
         LogOnce(LOG_LEVEL_INFO, DescribeConnectFailure(result));
      return false;
   }

#ifndef _WIN32
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
   // A new session gets only the latest game committed while no daemon listened, from its
   // start: batches without the game_start before them mean nothing to it.
   m_source.TakeGames(m_games);
   size_t keepFrom = m_games.size();
   for (size_t i = m_games.size(); i-- > 0;)
      if (!m_games[i].batch.ops.empty() && m_games[i].batch.ops.front().kind == Wire::GAME_START)
      {
         keepFrom = i;
         break;
      }
   if (keepFrom != 0)
   {
      Log(LOG_LEVEL_INFO, "Socket: dropped " + std::to_string(keepFrom) + " Game batches committed while disconnected");
      m_games.erase(m_games.begin(), m_games.begin() + static_cast<std::ptrdiff_t>(keepFrom));
   }
   return true;
}

SocketWorker::ConnectResult SocketWorker::ConnectNonBlocking(uintptr_t socket, const void* addr, int addrLen)
{
   const socket_t s = static_cast<socket_t>(socket);
   if (connect(s, static_cast<const sockaddr*>(addr), addrLen) == 0)
      return { ConnectOutcome::Connected, 0 };

   int error = SOCKET_ERRNO;
#ifdef _WIN32
   const bool inProgress = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
   // Linux returns EAGAIN from a nonblocking AF_UNIX connect when the listen queue is
   // full: the connection was never started, so there is nothing to wait for.
   if (error == EAGAIN || error == EWOULDBLOCK)
      return { ConnectOutcome::NotAccepting, error };
   const bool inProgress = error == EINPROGRESS;
#endif

   if (inProgress)
   {
      const auto deadline = Clock::now() + std::chrono::milliseconds(CONNECT_TIMEOUT_MS);
      bool timedOut = false;
      for (;;)
      {
         if (!m_running)
            return { ConnectOutcome::Stopped, 0 };
         const int left = MillisUntil(deadline);
         if (left == 0)
         {
            timedOut = true;
            break;
         }
#ifdef _WIN32
         WSAPOLLFD pfd { };
         pfd.fd = s;
         pfd.events = POLLWRNORM;
         const int rc = WSAPoll(&pfd, 1, left < POLL_SLICE_MS ? left : POLL_SLICE_MS);
#else
         pollfd pfd { };
         pfd.fd = s;
         pfd.events = POLLOUT;
         const int rc = poll(&pfd, 1, left < POLL_SLICE_MS ? left : POLL_SLICE_MS);
         if (rc < 0 && errno == EINTR)
            continue;
#endif
         if (rc < 0)
            return { ConnectOutcome::Failed, SOCKET_ERRNO };
         if (rc > 0)
            break;
      }
      int soError = 0;
#ifdef _WIN32
      int soLen = static_cast<int>(sizeof(soError));
      const int got = getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
#else
      socklen_t soLen = sizeof(soError);
      const int got = getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &soLen);
#endif
      if (got != 0)
         return { ConnectOutcome::Failed, SOCKET_ERRNO };
      if (soError == 0)
      {
         if (!timedOut)
            return { ConnectOutcome::Connected, 0 };
         // Still pending. Windows before 10 2004 never reports a failed connect to
         // WSAPoll, so an absent path can end up here too.
#ifdef _WIN32
         const int timeoutCode = WSAETIMEDOUT;
#else
         const int timeoutCode = ETIMEDOUT;
#endif
         return { PathExists() ? ConnectOutcome::NotAccepting : ConnectOutcome::NoSuchPath, timeoutCode };
      }
      error = soError;
   }

   // Decided by looking at the path, not the errno: that holds on every platform,
   // including Windows, where the code for a missing AF_UNIX path is not measured.
#ifdef _WIN32
   const bool refused = error == WSAECONNREFUSED;
#else
   const bool refused = error == ECONNREFUSED;
#endif
   if (!PathExists())
      return { ConnectOutcome::NoSuchPath, error };
   if (refused)
      return { ConnectOutcome::Refused, error };
   return { ConnectOutcome::Failed, error };
}

bool SocketWorker::PathExists() const
{
#ifdef _WIN32
   // Only a definite "not there" counts as absent; access denied means something is.
   const std::wstring wpath = ToWide(m_socketPath);
   if (wpath.empty() || GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES)
      return true;
   const DWORD e = GetLastError();
   return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
#else
   // stat, not lstat: a symlink to a socket that is gone is as absent as no file.
   struct stat st { };
   return stat(m_socketPath.c_str(), &st) == 0 || errno != ENOENT;
#endif
}

std::string SocketWorker::DescribeConnectFailure(const ConnectResult& result) const
{
   const std::string code = result.error == 0 ? std::string()
      : " (" + std::system_category().message(result.error) + ", error " + std::to_string(result.error) + ')';
   switch (result.outcome)
   {
   case ConnectOutcome::NoSuchPath:
      return "Socket: nothing at " + m_socketPath + code + ", the daemon is not running; retrying every second";
   case ConnectOutcome::Refused:
      // macOS reports a full listen queue this way too, so a live daemon stays possible.
      return "Socket: " + m_socketPath + " refused the connection" + code
         + ": a stale socket from a daemon that exited, or a daemon that is not accepting; retrying every second";
   case ConnectOutcome::NotAccepting:
      return "Socket: a daemon is listening at " + m_socketPath + " but not accepting connections" + code
         + "; retrying every second";
   default:
      return "Socket: cannot connect to " + m_socketPath + code + "; retrying every second";
   }
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
   {
      Log(LOG_LEVEL_INFO, "Socket: session ended");
      // A shown-until-hidden overlay must not outlive the daemon that drew it.
      if (m_overlay != nullptr)
         m_overlay->OverlayClear();
      NoSession(std::string());
   }
}

void SocketWorker::PostStatus()
{
   if (m_status != nullptr)
      m_status->Post(m_statusRecord);
}

void SocketWorker::NoSession(const std::string& reason)
{
   if (!m_statusRecord.inSession && m_statusRecord.reason == reason)
      return;
   // Counted from the loss, so a refusal while already out of session keeps the clock.
   if (m_statusRecord.inSession)
      m_statusRecord.since = Clock::now();
   m_statusRecord.inSession = false;
   m_statusRecord.haveStatus = false;
   m_statusRecord.status = { };
   m_statusRecord.reason = reason;
   PostStatus();
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

bool SocketWorker::SendWireMessage(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload,
   const std::vector<Wire::SensitiveSpan>& sensitive)
{
   const std::vector<uint8_t> bytes = Wire::EncodeMessage(type, flags, seq, payload);
   if (bytes.empty())
   {
      Log(LOG_LEVEL_ERROR, "Socket: refusing to send an oversized "s + Wire::TypeName(type) + " message");
      return false;
   }
   if (sensitive.empty())
      m_wireDump.Sent(bytes.data(), bytes.size());
   else
   {
      std::vector<SensitiveSpan> framed = sensitive;
      for (SensitiveSpan& span : framed)
         span.offset += Wire::LENGTH_BYTES + Wire::HEADER_BYTES;
      m_wireDump.Sent(bytes.data(), bytes.size(), framed);
   }
   return SendAll(bytes.data(), bytes.size(), SEND_TIMEOUT_MS);
}

bool SocketWorker::SendError(uint16_t type, uint32_t seq, uint16_t code, const std::string& reason)
{
   Wire::ErrorPayload e;
   e.code = code;
   e.reason = reason;
   return SendWireMessage(type, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, seq, Wire::Encode(e));
}

bool SocketWorker::Exchange(uint16_t type, const std::vector<uint8_t>& payload, int timeoutMs, Wire::Message& reply,
   const std::vector<Wire::SensitiveSpan>& sensitive)
{
   const uint32_t seq = ++m_seq;
   if (!SendWireMessage(type, 0, seq, payload, sensitive))
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
   {
      NoSession(std::string());
      return false;
   }

   Wire::Hello hello;
   hello.protoMajor = Wire::PROTO_MAJOR;
   hello.protoMinor = Wire::PROTO_MINOR;
   hello.pluginVersion = m_config.pluginVersion;
   hello.pluginApiCommit = m_config.pluginApiCommit;
   hello.token = token;
   hello.instanceId = m_instanceId;
   // memory_write is advertised whenever reads are: whether the host can write is only known
   // from the first Write, which answers unsupported_operation on a libpinmame without #634.
   hello.capabilities = Wire::CAP_IDENTIFY_FRAMES
      | (m_memory != nullptr ? Wire::CAP_MEMORY_READ | Wire::CAP_MEMORY_WRITE : 0u)
      | (m_overlay != nullptr ? Wire::CAP_OVERLAY : 0u)
      | (m_config.authoredTables ? Wire::CAP_AUTHORED_TABLES : 0u);

   // Hello's vpx_revision is the build the plugin API headers were pinned to.
   // The plugin API exposes no runtime version, so this is what the plugin
   // knows rather than what is actually running; the daemon logs it as such.
   TakeDeclare();
   hello.vpxRevision = m_current.declare.vpxRevision;

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
      // fix, so they are reported once at error level and the session stops. The
      // reason is the no-session notice's support text (REQ-6.11).
      LogOnce(LOG_LEVEL_ERROR, "Socket: daemon refused Hello: "s + Wire::ErrorName(e.code) + " - " + e.reason);
      NoSession(Wire::ErrorName(e.code) + (e.reason.empty() ? std::string() : ": " + e.reason));
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
   m_loggedNvRam = false;
   m_peerMinor = ack.protoMinor;
   m_peerCapabilities = ack.capabilities;
   m_loggedNoAuthored = false;
   // The daemon sends Status after the first Declare, on every connection.
   m_statusRecord = { };
   m_statusRecord.inSession = true;
   PostStatus();
   // Every session starts with no subscription (design.md, subscription lifetime).
   if (m_memory != nullptr)
      m_memory->Clear();
   Log(LOG_LEVEL_INFO, "Socket: connected to daemon " + ack.daemonVersion + " (protocol "
      + std::to_string(ack.protoMajor) + '.' + std::to_string(ack.protoMinor)
      + ", session " + std::to_string(ack.sessionId)
      + (AuthoredAllowed() ? ", authored tables" : "") + ')');
   return true;
}

bool SocketWorker::AuthoredAllowed() const
{
   return m_peerMinor >= 1 && (m_peerCapabilities & Wire::CAP_AUTHORED_TABLES) != 0;
}

void SocketWorker::TakeDeclare()
{
   if (std::unique_ptr<DeclareSnapshot> posted = m_source.TakeDeclare())
      m_current = std::move(*posted);
}

bool SocketWorker::MaybeDeclare()
{
   TakeDeclare();
   // A table change: what the old table committed goes out before the Declare that ends it,
   // so a game_end flushed just before the table stopped is not lost. The source posts a
   // batch before the Declare that follows it, so taking that Declare has made it visible.
   if (m_haveDeclared && m_current.epoch != m_declared.epoch && !SendGames())
      return false;
   if (m_haveDeclared && m_current == m_declared)
      return true;
   const DeclareSnapshot now = m_current;

   Wire::Declare sent = now.declare;
   if (sent.tableKind == Wire::TABLE_AUTHORED && !AuthoredAllowed())
   {
      if (!m_loggedNoAuthored)
      {
         Log(LOG_LEVEL_WARN, "Socket: the daemon does not serve authored tables, so this table is declared without "
            "its Scorbit machine and its game calls are dropped");
         m_loggedNoAuthored = true;
      }
      sent.tableKind = Wire::TABLE_ROM;
      sent.machineUuid.clear();
      sent.tableFullPath.clear();
      sent.vpxAppPath.clear();
      sent.prefsPath.clear();
   }
   std::vector<Wire::SensitiveSpan> sensitive;
   const std::vector<uint8_t> payload = Wire::Encode(sent, m_peerMinor, &sensitive);

   // A Declare clears the subscription on both sides; cleared before it goes out,
   // so a Poll crossing it never serves the previous ROM's addresses.
   if (m_memory != nullptr)
      m_memory->Clear();

   Wire::Message reply;
   if (!Exchange(Wire::TYPE_DECLARE, payload, DECLARE_TIMEOUT_MS, reply, sensitive))
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
   m_gameLogged = 0;
   // The paths are never logged (REQ-12.6).
   const Wire::Declare& d = sent;
   Log(LOG_LEVEL_INFO, "Socket: declared romId=" + (d.romId.empty() ? "(none)" : d.romId)
      + (d.tableKind == Wire::TABLE_AUTHORED ? " authored machine=" + d.machineUuid : std::string())
      + " display=" + std::to_string(d.width) + 'x' + std::to_string(d.height)
      + '/' + std::to_string(d.shades) + " shades"
      + " generation=" + std::to_string(d.sourceGeneration)
      + " format=" + (d.identifyFormat.empty() ? "(none)" : d.identifyFormat));
   return true;
}

bool SocketWorker::SendGames()
{
   m_source.TakeGames(m_games);
   while (!m_games.empty() && m_haveDeclared)
   {
      GamePost& front = m_games.front();
      if (front.epoch > m_declared.epoch)
         return true;  // its Declare has not gone out yet
      const bool authored = m_declared.declare.tableKind == Wire::TABLE_AUTHORED && AuthoredAllowed();
      if (front.epoch == m_declared.epoch && !authored)
      {
         // The authored Declare this batch follows may have been posted after this loop
         // last looked; it goes out first.
         TakeDeclare();
         if (!(m_current == m_declared))
            return true;
      }
      const std::vector<uint8_t> payload = authored ? Wire::Encode(front.batch) : std::vector<uint8_t> { };
      if (front.epoch < m_declared.epoch || payload.empty())
      {
         if ((m_gameLogged & 1u) == 0)
         {
            Log(LOG_LEVEL_WARN, "Socket: dropped a Game: its table is no longer declared, or not as an authored table");
            m_gameLogged |= 1u;
         }
         m_games.pop_front();
         continue;
      }

      Wire::Message reply;
      if (!Exchange(Wire::TYPE_GAME, payload, GAME_TIMEOUT_MS, reply))
         return false;
      m_games.pop_front();
      if (!reply.header.IsError())
         continue;
      // Logged once per code per Declare and otherwise carried on: the daemon drops what it
      // must (too_large drops the whole game), and a later game can still score.
      Wire::ErrorPayload e;
      Wire::Decode(reply.payload, e);
      const uint32_t bit = 1u << (e.code < 31 ? e.code : 31);
      if ((m_gameLogged & bit) == 0)
      {
         Log(LOG_LEVEL_WARN, "Socket: daemon refused Game: "s + Wire::ErrorName(e.code) + " - " + e.reason);
         m_gameLogged |= bit;
      }
   }
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
      if (!MaybeDeclare() || !SendGames())
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
   SendWireMessage(Wire::TYPE_BYE, 0, ++m_seq, Wire::Encode(Wire::Bye { "plugin unloading" }));
   Log(LOG_LEVEL_INFO, "Socket: sent Bye");
}

bool SocketWorker::AnswerWrite(const Wire::Message& msg)
{
   Wire::WriteRequest req;
   if (!Wire::Decode(msg.payload, req))
   {
      Log(LOG_LEVEL_ERROR, "Socket: malformed Write request, closing");
      SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "Write request payload");
      return false;
   }
   Wire::WriteReply out;
   const MemoryBridge::Result result = m_memory->Write(req.ops, out, WRITE_BUDGET_MS, m_running);
   if (result == MemoryBridge::Result::Unsupported)
      LogOnce(LOG_LEVEL_WARN, "Socket: this PinMAME applies no memory writes (needs pinmame #634); answering unsupported_operation");
   if (result == MemoryBridge::Result::OutOfRange)
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_OUT_OF_RANGE,
         "the Write names no ops, or an op runs past the end of the address space");
   if (result != MemoryBridge::Result::Ok)
      return SendMemoryError(msg, result);
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(out));
}

// Informational (design.md): the daemon puts the unlock inside each Write, so nothing here
// changes how a Write is applied. It is served rather than refused, because code 1 in answer
// to any slice 2 type ends the session's memory traffic.
bool SocketWorker::AnswerNvRam(const Wire::Message& msg)
{
   Wire::NvRamParameters nvram;
   if (!Wire::Decode(msg.payload, nvram))
   {
      Log(LOG_LEVEL_ERROR, "Socket: malformed NvRam request, closing");
      SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "NvRam request payload");
      return false;
   }
   if (!m_loggedNvRam)
   {
      char line[160];
      std::snprintf(line, sizeof(line), "Socket: NvRam window 0x%04x+0x%x, unlock 0x%02x at 0x%04x, lock 0x%02x",
         static_cast<unsigned>(nvram.start), static_cast<unsigned>(nvram.length),
         static_cast<unsigned>(nvram.unlockValue), static_cast<unsigned>(nvram.unlockAddress),
         static_cast<unsigned>(nvram.lockValue));
      Log(LOG_LEVEL_INFO, line);
      m_loggedNvRam = true;
   }
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, { });
}

bool SocketWorker::Dispatch(const Wire::Message& msg)
{
   switch (msg.header.type)
   {
   case Wire::TYPE_FRAME:
      return AnswerFrame(msg);
   case Wire::TYPE_PING:
      return AnswerPing(msg);
   case Wire::TYPE_SUBSCRIBE:
      if (m_memory != nullptr)
         return AnswerSubscribe(msg);
      break;
   case Wire::TYPE_POLL:
      if (m_memory != nullptr)
         return AnswerPoll(msg);
      break;
   case Wire::TYPE_READ_DIRECT:
      if (m_memory != nullptr)
         return AnswerReadDirect(msg);
      break;
   case Wire::TYPE_WRITE:
      if (m_memory != nullptr)
         return AnswerWrite(msg);
      break;
   case Wire::TYPE_NVRAM:
      if (m_memory != nullptr)
         return AnswerNvRam(msg);
      break;
   case Wire::TYPE_OVERLAY:
      if (m_overlay != nullptr)
         return AnswerOverlay(msg);
      break;
   case Wire::TYPE_STATUS:
      if (m_status != nullptr)
         return AnswerStatus(msg);
      break;
   case Wire::TYPE_BYE:
   {
      Wire::Bye bye;
      Wire::Decode(msg.payload, bye);
      Log(LOG_LEVEL_INFO, "Socket: daemon said Bye" + (bye.reason.empty() ? std::string() : ": " + bye.reason));
      return false;
   }
   default:
      break;
   }
   // Everything else, including the memory reads, overlay and status when this
   // worker was given nothing to serve them with, is refused explicitly so the daemon learns at once
   // what this build serves instead of waiting out a timeout.
   Log(LOG_LEVEL_DEBUG, "Socket: refusing "s + Wire::TypeName(msg.header.type) + " request");
   return SendError(msg.header.type, msg.header.seq, Wire::ERR_UNSUPPORTED_TYPE,
      std::string(Wire::TypeName(msg.header.type)) + " is not served by this plugin");
}

// Served here, never through the API thread, so never busy. A malformed request is answered
// and the session kept: its framing is intact, and an animation then loses one frame, not
// the session.
bool SocketWorker::AnswerOverlay(const Wire::Message& msg)
{
   if (!m_haveDeclared)
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_NOT_DECLARED, "Overlay before the first Declare");
   if (m_declared.declare.shades == 0 || !m_overlay->OverlaySurface())
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_UNSUPPORTED_OPERATION, "no DMD source");

   Wire::OverlayRequest req;
   Wire::OverlayControl control;
   const bool decoded = Wire::Decode(msg.payload, req)
      && (req.op == Wire::OVERLAY_OP_UPLOAD || (req.op == Wire::OVERLAY_OP_CONTROL && Wire::Decode(req.payload, control)));
   if (!decoded)
   {
      Log(LOG_LEVEL_WARN, "Socket: malformed Overlay request (op " + std::to_string(req.op) + ", "
         + std::to_string(msg.payload.size()) + " bytes)");
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "unknown op, or a payload shorter than its fields");
   }

   if (req.op == Wire::OVERLAY_OP_UPLOAD)
   {
      switch (m_overlay->OverlayUpload(req.payload.data(), req.payload.size()))
      {
      case Overlay::State::Upload::Malformed:
         Log(LOG_LEVEL_WARN, "Socket: malformed Overlay upload, " + std::to_string(req.payload.size()) + " bytes");
         return SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "upload shorter than its width and height");
      case Overlay::State::Upload::Ignored:
         // Acknowledged, as the probe acknowledges it; the previous bitmap stays.
         LogOnce(LOG_LEVEL_WARN, "Socket: ignored an Overlay upload of " + std::to_string(req.payload[0]) + 'x'
            + std::to_string(req.payload[1]) + ", outside 10x10 to 192x64; the previous overlay stays");
         break;
      case Overlay::State::Upload::Stored: // not logged: an animation uploads every frame
         break;
      }
   }
   else
   {
      const bool enable = (control.control & Wire::OVERLAY_CONTROL_ENABLE) != 0;
      m_overlay->OverlayControl(enable, control.durationMs);
      Log(LOG_LEVEL_DEBUG, !enable ? "Socket: overlay hidden"s
         : control.durationMs == 0 ? "Socket: overlay shown until hidden"s
         : "Socket: overlay shown for " + std::to_string(control.durationMs) + " ms");
   }
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, { });
}

// Answered at once. The notification is the API thread's to post, so the record goes
// through the latch; this side only logs it.
bool SocketWorker::AnswerStatus(const Wire::Message& msg)
{
   if (!m_haveDeclared)
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_NOT_DECLARED, "Status before the first Declare");
   Wire::Status status;
   if (!Wire::Decode(msg.payload, status))
   {
      Log(LOG_LEVEL_WARN, "Socket: malformed Status request, " + std::to_string(msg.payload.size()) + " bytes");
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED,
         "a short payload, or support_text over 256 bytes");
   }
   Log(LOG_LEVEL_INFO, "Socket: daemon status " + StatusNotifier::Describe(status));
   m_statusRecord.haveStatus = true;
   m_statusRecord.status = std::move(status);
   PostStatus();
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, { });
}

bool SocketWorker::SendMemoryError(const Wire::Message& msg, MemoryBridge::Result result)
{
   switch (result)
   {
   case MemoryBridge::Result::NoGame:
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_NO_GAME, "no ROM is running");
   case MemoryBridge::Result::OutOfRange:
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_OUT_OF_RANGE, "range is outside readable memory");
   case MemoryBridge::Result::TooLarge:
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_TOO_LARGE, "request exceeds the slice 2 limits");
   case MemoryBridge::Result::Unsupported:
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_UNSUPPORTED_OPERATION,
         "this PinMAME applies no memory writes (needs pinmame #634)");
   default:
      return SendError(msg.header.type, msg.header.seq, Wire::ERR_BUSY, "memory was not serviced in time, ask again");
   }
}

bool SocketWorker::AnswerSubscribe(const Wire::Message& msg)
{
   Wire::Subscription req;
   if (!Wire::Decode(msg.payload, req))
   {
      Log(LOG_LEVEL_ERROR, "Socket: malformed Subscribe request, closing");
      SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "Subscribe request payload");
      return false;
   }
   Wire::Subscription accepted;
   const MemoryBridge::Result result = m_memory->Subscribe(req.ranges, accepted.ranges, MEMORY_BUDGET_MS, m_running);
   if (result != MemoryBridge::Result::Ok)
      return SendMemoryError(msg, result);
   if (accepted.ranges.size() != req.ranges.size())
      Log(LOG_LEVEL_DEBUG, "Socket: subscribed " + std::to_string(accepted.ranges.size()) + " of "
         + std::to_string(req.ranges.size()) + " ranges; the rest are not readable in full");
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(accepted));
}

bool SocketWorker::AnswerPoll(const Wire::Message& msg)
{
   Wire::PollReply out;
   const MemoryBridge::Result result = m_memory->Poll(out);
   if (result != MemoryBridge::Result::Ok)
      return SendMemoryError(msg, result);
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(out));
}

bool SocketWorker::AnswerReadDirect(const Wire::Message& msg)
{
   Wire::ReadDirectRequest req;
   if (!Wire::Decode(msg.payload, req))
   {
      Log(LOG_LEVEL_ERROR, "Socket: malformed ReadDirect request, closing");
      SendError(msg.header.type, msg.header.seq, Wire::ERR_MALFORMED, "ReadDirect request payload");
      return false;
   }
   Wire::ReadDirectReply out;
   const MemoryBridge::Result result = m_memory->ReadDirect(req.address, req.length, out, MEMORY_BUDGET_MS, m_running);
   if (result != MemoryBridge::Result::Ok)
      return SendMemoryError(msg, result);
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(out));
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
   return SendWireMessage(msg.header.type, Wire::FLAG_RESPONSE, msg.header.seq, payload);
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
   // What this side knows, as for a Write: served, unless the host has shown it applies none.
   pong.writesSupported = m_memory != nullptr && !m_memory->WritesUnsupported() ? 1 : 0;
   return SendWireMessage(Wire::TYPE_PONG, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(pong));
}

}
