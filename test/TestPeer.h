// license:GPLv3+

#pragma once

// A stand in for the daemon: it listens where SocketCable would, writes a token
// where SocketCable would, and speaks the protocol by hand. Nothing here shares
// code with the worker's socket handling, so a test that passes says the two
// implementations agree rather than that one implementation is self consistent.
//
// POSIX only. The Windows path of the worker is written but compiles nowhere in
// CI today, so this peer does not pretend to exercise it.

#include "WireProtocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace ScorbitTest
{

class TestPeer final
{
public:
   explicit TestPeer(const std::string& dir, const std::string& token =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
      : m_dir(dir)
      , m_socketPath(dir + "/vpx.sock")
      , m_tokenPath(dir + "/vpx.token")
      , m_token(token)
   {
      mkdir(m_dir.c_str(), 0700);
      unlink(m_socketPath.c_str());
      WriteToken(m_token);

      m_listen = socket(AF_UNIX, SOCK_STREAM, 0);
      if (m_listen < 0)
         return;

      sockaddr_un addr { };
      addr.sun_family = AF_UNIX;
      if (m_socketPath.size() >= sizeof(addr.sun_path))
      {
         fprintf(stderr, "TestPeer: socket path is too long: %s\n", m_socketPath.c_str());
         close(m_listen);
         m_listen = -1;
         return;
      }
      memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size());
      if (bind(m_listen, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0
         || listen(m_listen, 4) != 0)
      {
         perror("TestPeer bind/listen");
         close(m_listen);
         m_listen = -1;
      }
   }

   ~TestPeer()
   {
      CloseClient();
      if (m_listen >= 0)
         close(m_listen);
      unlink(m_socketPath.c_str());
      unlink(m_tokenPath.c_str());
      rmdir(m_dir.c_str());
   }

   TestPeer(const TestPeer&) = delete;
   TestPeer& operator=(const TestPeer&) = delete;

   bool Listening() const { return m_listen >= 0; }
   const std::string& SocketPath() const { return m_socketPath; }
   const std::string& TokenPath() const { return m_tokenPath; }
   const std::string& Token() const { return m_token; }

   void WriteToken(const std::string& token)
   {
      m_token = token;
      FILE* f = fopen(m_tokenPath.c_str(), "wb");
      if (f == nullptr)
         return;
      fwrite(token.data(), 1, token.size(), f);
      fputc('\n', f);
      fclose(f);
      chmod(m_tokenPath.c_str(), 0600);
   }

   void RemoveToken()
   {
      unlink(m_tokenPath.c_str());
      m_token.clear();
   }

   bool Accept(int timeoutMs)
   {
      CloseClient();
      if (!Wait(m_listen, POLLIN, timeoutMs))
         return false;
      m_client = accept(m_listen, nullptr, nullptr);
      return m_client >= 0;
   }

   void CloseClient()
   {
      if (m_client >= 0)
      {
         close(m_client);
         m_client = -1;
      }
   }

   bool Send(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload)
   {
      const std::vector<uint8_t> bytes = Scorbit::Wire::EncodeMessage(type, flags, seq, payload);
      return !bytes.empty() && SendRaw(bytes);
   }

   bool SendRaw(const std::vector<uint8_t>& bytes)
   {
      size_t sent = 0;
      while (sent < bytes.size())
      {
         const ssize_t n = write(m_client, bytes.data() + sent, bytes.size() - sent);
         if (n <= 0)
            return false;
         sent += static_cast<size_t>(n);
      }
      return true;
   }

   // Reads one message. False on timeout or a closed peer.
   bool Read(Scorbit::Wire::Message& out, int timeoutMs)
   {
      uint8_t lengthBytes[4];
      if (!ReadExact(lengthBytes, sizeof(lengthBytes), timeoutMs))
         return false;
      const uint32_t length = static_cast<uint32_t>(lengthBytes[0])
         | (static_cast<uint32_t>(lengthBytes[1]) << 8)
         | (static_cast<uint32_t>(lengthBytes[2]) << 16)
         | (static_cast<uint32_t>(lengthBytes[3]) << 24);
      if (length < 8 || length > (1u << 20))
         return false;
      std::vector<uint8_t> body(length);
      if (!ReadExact(body.data(), body.size(), timeoutMs))
         return false;
      return Scorbit::Wire::DecodeBody(body.data(), body.size(), out);
   }

   // Reads one request of the given type, answering any Ping that arrives
   // first so an idle tick does not derail a scripted exchange.
   bool ReadOfType(uint16_t type, Scorbit::Wire::Message& out, int timeoutMs)
   {
      for (int i = 0; i < 16; i++)
      {
         if (!Read(out, timeoutMs))
            return false;
         if (out.header.type == Scorbit::Wire::TYPE_PING && !out.header.IsResponse())
         {
            Send(Scorbit::Wire::TYPE_PONG, Scorbit::Wire::FLAG_RESPONSE, out.header.seq,
               Scorbit::Wire::Encode(Scorbit::Wire::Pong { }));
            continue;
         }
         return out.header.type == type;
      }
      return false;
   }

   // True when the peer sees the plugin hang up within the timeout.
   bool WaitForClose(int timeoutMs)
   {
      if (!Wait(m_client, POLLIN, timeoutMs))
         return false;
      uint8_t b = 0;
      return read(m_client, &b, 1) == 0;
   }

   // True when nothing at all arrives within the timeout.
   bool Silent(int timeoutMs)
   {
      return !Wait(m_client, POLLIN, timeoutMs);
   }

private:
   static bool Wait(int fd, short events, int timeoutMs)
   {
      if (fd < 0)
         return false;
      pollfd pfd { };
      pfd.fd = fd;
      pfd.events = events;
      const int rc = poll(&pfd, 1, timeoutMs);
      return rc > 0;
   }

   bool ReadExact(uint8_t* dst, size_t size, int timeoutMs)
   {
      size_t got = 0;
      while (got < size)
      {
         if (!Wait(m_client, POLLIN, timeoutMs))
            return false;
         const ssize_t n = read(m_client, dst + got, size - got);
         if (n <= 0)
            return false;
         got += static_cast<size_t>(n);
      }
      return true;
   }

   std::string m_dir;
   std::string m_socketPath;
   std::string m_tokenPath;
   std::string m_token;
   int m_listen = -1;
   int m_client = -1;
};

}
