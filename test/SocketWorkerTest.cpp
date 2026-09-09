// license:GPLv3+

// Drives a real SocketWorker against the scripted peer in TestPeer.h: the whole
// session from connect to Bye, without Visual Pinball and without a daemon.

#include "SocketWorker.h"

#include "Check.h"
#include "TestPeer.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using namespace Scorbit;

namespace
{

constexpr int WAIT_MS = 5000;

class FakeSource final : public ISessionSource
{
public:
   void GetDeclare(DeclareSnapshot& out) override
   {
      std::lock_guard lock(m_mutex);
      out = m_declare;
   }

   void GetFrame(FrameSnapshot& out) override
   {
      std::lock_guard lock(m_mutex);
      out = m_frame;
   }

   void GetSession(SessionSnapshot& out) override
   {
      std::lock_guard lock(m_mutex);
      out = m_session;
   }

   void Set(const DeclareSnapshot& declare, const FrameSnapshot& frame)
   {
      std::lock_guard lock(m_mutex);
      m_declare = declare;
      m_frame = frame;
   }

   void SetSession(const SessionSnapshot& session)
   {
      std::lock_guard lock(m_mutex);
      m_session = session;
   }

private:
   std::mutex m_mutex;
   DeclareSnapshot m_declare;
   FrameSnapshot m_frame;
   SessionSnapshot m_session;
};

std::string MakeTempDir()
{
   // Kept short: the whole path has to fit sockaddr_un's 104 bytes on macOS.
   char tmpl[] = "/tmp/scorbit-sw-XXXXXX";
   const char* dir = mkdtemp(tmpl);
   return dir != nullptr ? std::string(dir) : std::string();
}

void Fill(FrameSnapshot& frame, uint32_t frameId, uint32_t generation, uint16_t w, uint16_t h, uint8_t shades)
{
   frame.hasSource = true;
   frame.frameId = frameId;
   frame.generation = generation;
   frame.width = w;
   frame.height = h;
   frame.shades = shades;
   frame.pixels.resize(static_cast<size_t>(w) * h);
   for (size_t i = 0; i < frame.pixels.size(); i++)
      frame.pixels[i] = static_cast<uint8_t>((i + frameId) % shades);
}

DeclareSnapshot MakeDeclare(const std::string& romId, uint16_t w, uint16_t h, uint8_t shades, uint32_t generation)
{
   DeclareSnapshot d;
   d.declare.romId = romId;
   d.declare.tablePath = "/tables/t.vpx";
   d.declare.vpxVersion = "VPinballX_BGFX-5589";
   d.declare.vpxRevision = "5589";
   d.declare.width = w;
   d.declare.height = h;
   d.declare.shades = shades;
   d.declare.sourceGeneration = generation;
   d.declare.identifyFormat = shades == 0 ? "" : (shades == 16 ? "BITPLANE4" : "BITPLANE2");
   return d;
}

SocketWorkerConfig MakeConfig(const ScorbitTest::TestPeer& peer)
{
   SocketWorkerConfig cfg;
   cfg.socketPath = peer.SocketPath();
   cfg.tokenPath = peer.TokenPath();
   cfg.pluginVersion = "0.1.0";
   cfg.pluginApiCommit = "0bc9838ed5f1bbac869efdfb6e785b829a541d3f";
   return cfg;
}

SocketWorker::LogFn Quiet()
{
   const bool verbose = getenv("SCORBIT_TEST_VERBOSE") != nullptr;
   return [verbose](int level, const std::string& message)
   {
      if (verbose)
         fprintf(stderr, "  [worker %d] %s\n", level, message.c_str());
   };
}

// Runs the handshake the daemon side would: read Hello, answer HelloAck, read
// the first Declare and answer it. Returns the Declare that arrived.
bool Handshake(ScorbitTest::TestPeer& peer, Wire::Hello& hello, Wire::Declare& declare)
{
   Wire::Message msg;
   if (!peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS))
      return false;
   if (!Wire::Decode(msg.payload, hello))
      return false;

   Wire::HelloAck ack;
   ack.protoMajor = Wire::PROTO_MAJOR;
   ack.protoMinor = Wire::PROTO_MINOR;
   ack.daemonVersion = "scorbitd-test";
   ack.capabilities = Wire::CAP_IDENTIFY_FRAMES;
   ack.sessionId = 77;
   if (!peer.Send(Wire::TYPE_HELLO_ACK, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(ack)))
      return false;

   if (!peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS))
      return false;
   if (!Wire::Decode(msg.payload, declare))
      return false;
   return peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { });
}

// The whole happy path, then the two ways a session ends badly.
void TestSession()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;

   ScorbitTest::TestPeer peer(dir);
   CHECK(peer.Listening());
   if (!peer.Listening())
      return;

   FakeSource source;
   source.Set(MakeDeclare("ij_l7", 128, 32, 4, 1), { });
   {
      FrameSnapshot frame;
      Fill(frame, 10, 1, 128, 32, 4);
      source.Set(MakeDeclare("ij_l7", 128, 32, 4, 1), frame);
   }
   source.SetSession(SessionSnapshot { 12345, true });

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();

   CHECK(peer.Accept(WAIT_MS));

   // --- Hello ------------------------------------------------------------
   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));
   CHECK(hello.protoMajor == Wire::PROTO_MAJOR);
   CHECK(hello.protoMinor == Wire::PROTO_MINOR);
   CHECK(hello.pluginVersion == "0.1.0");
   CHECK(hello.pluginApiCommit == "0bc9838ed5f1bbac869efdfb6e785b829a541d3f");
   CHECK_MSG(hello.token == peer.Token(), "token '" + hello.token + "'");
   CHECK(hello.capabilities == Wire::CAP_IDENTIFY_FRAMES);
   CHECK(hello.instanceId != 0);
   CHECK(hello.vpxRevision == "5589");

   // --- Declare ----------------------------------------------------------
   CHECK(declare.romId == "ij_l7");
   CHECK(declare.tablePath == "/tables/t.vpx");
   CHECK(declare.width == 128 && declare.height == 32 && declare.shades == 4);
   CHECK(declare.sourceGeneration == 1);
   CHECK(declare.identifyFormat == "BITPLANE2");

   // --- Frame, nothing seen yet -----------------------------------------
   Wire::Message msg;
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 1000, Wire::Encode(Wire::FrameRequest { 0, 0 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(msg.header.IsResponse() && !msg.header.IsError() && msg.header.seq == 1000);
   Wire::FrameReply reply;
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.frameId == 10 && reply.sourceGeneration == 1);
   CHECK(reply.width == 128 && reply.height == 32 && reply.shades == 4);
   CHECK(reply.hasPixels == 1);
   CHECK(reply.pixels.size() == 128u * 32u);
   CHECK(reply.pixels[0] == 10 % 4 && reply.pixels[1] == 11 % 4);

   // --- Frame, already up to date ---------------------------------------
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 1001, Wire::Encode(Wire::FrameRequest { 10, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK_MSG(reply.hasPixels == 0, "an unchanged frame must not resend pixels");
   CHECK(reply.frameId == 10 && reply.sourceGeneration == 1);
   CHECK(reply.width == 128 && reply.height == 32 && reply.shades == 4);

   // --- Ping -------------------------------------------------------------
   CHECK(peer.Send(Wire::TYPE_PING, 0, 1002, { }));
   CHECK(peer.ReadOfType(Wire::TYPE_PONG, msg, WAIT_MS));
   CHECK(msg.header.IsResponse() && msg.header.seq == 1002);
   Wire::Pong pong;
   CHECK(Wire::Decode(msg.payload, pong));
   CHECK(pong.renderedFrame == 12345);
   CHECK(pong.playerRunning == 1);
   CHECK_MSG(pong.writesSupported == 0, "slice 1 serves no writes");

   // --- Reserved and unknown types are refused, not ignored --------------
   const uint16_t refused[] = { Wire::TYPE_SUBSCRIBE, Wire::TYPE_POLL, Wire::TYPE_READ_DIRECT,
      Wire::TYPE_WRITE, Wire::TYPE_NVRAM, Wire::TYPE_OVERLAY, Wire::TYPE_STATUS, 999 };
   uint32_t seq = 2000;
   for (const uint16_t type : refused)
   {
      CHECK(peer.Send(type, 0, seq, { }));
      CHECK_MSG(peer.ReadOfType(type, msg, WAIT_MS), std::string("error must echo type ") + std::to_string(type));
      CHECK(msg.header.IsResponse() && msg.header.IsError() && msg.header.seq == seq);
      Wire::ErrorPayload err;
      CHECK(Wire::Decode(msg.payload, err));
      CHECK_MSG(err.code == Wire::ERR_UNSUPPORTED_TYPE, "code " + std::to_string(err.code));
      seq++;
   }

   // --- A new source generation re-declares without being asked ----------
   {
      FrameSnapshot frame;
      Fill(frame, 1, 2, 192, 64, 16);
      source.Set(MakeDeclare("ij_l7", 192, 64, 16, 2), frame);
   }
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare second;
   CHECK(Wire::Decode(msg.payload, second));
   CHECK(second.width == 192 && second.height == 64 && second.shades == 16);
   CHECK(second.sourceGeneration == 2);
   CHECK(second.identifyFormat == "BITPLANE4");
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   // The generation moved, so the daemon's old frame id no longer counts as
   // up to date even though the numbers would otherwise match.
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 3000, Wire::Encode(Wire::FrameRequest { 1, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.hasPixels == 1 && reply.sourceGeneration == 2);
   CHECK(reply.pixels.size() == 192u * 64u);

   // --- No source is a steady state, not an error ------------------------
   source.Set(MakeDeclare("", 0, 0, 0, 3), FrameSnapshot { false, 0, 3, 0, 0, 0, { } });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare none;
   CHECK(Wire::Decode(msg.payload, none));
   CHECK(none.shades == 0 && none.identifyFormat.empty() && none.sourceGeneration == 3);
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 4000, Wire::Encode(Wire::FrameRequest { 0, 0 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.frameId == 0 && reply.hasPixels == 0 && reply.sourceGeneration == 3);
   CHECK(reply.width == 0 && reply.height == 0 && reply.shades == 0);

   // --- A truncated payload ends the session with error 7 ----------------
   std::vector<uint8_t> shortFrame = { 1, 2, 3 };
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 5000, shortFrame));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(msg.header.IsError() && msg.header.seq == 5000);
   Wire::ErrorPayload err;
   CHECK(Wire::Decode(msg.payload, err));
   CHECK_MSG(err.code == Wire::ERR_MALFORMED, "code " + std::to_string(err.code));
   CHECK_MSG(peer.WaitForClose(WAIT_MS), "a malformed request must close the session");

   // --- And the plugin comes back on its own -----------------------------
   CHECK_MSG(peer.Accept(WAIT_MS), "the worker must reconnect after a dropped session");
   Wire::Hello again;
   Wire::Declare declareAgain;
   CHECK(Handshake(peer, again, declareAgain));
   CHECK_MSG(again.instanceId == hello.instanceId, "instance_id is per plugin load, not per connection");

   // --- Unload says Bye --------------------------------------------------
   worker.Stop();
   CHECK(peer.ReadOfType(Wire::TYPE_BYE, msg, WAIT_MS));
   Wire::Bye bye;
   CHECK(Wire::Decode(msg.payload, bye));
   CHECK(!bye.reason.empty());
}

// A message that claims to be larger than the bound is refused and the session
// is dropped, rather than the worker trying to allocate for it.
void TestOversizedLength()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;

   ScorbitTest::TestPeer peer(dir);
   CHECK(peer.Listening());
   if (!peer.Listening())
      return;

   FakeSource source;
   source.Set(MakeDeclare("", 0, 0, 0, 0), { });

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));

   // 1 MiB plus one, as a bare length header with nothing behind it.
   const uint32_t length = (1u << 20) + 1;
   const std::vector<uint8_t> header = {
      static_cast<uint8_t>(length & 0xFF), static_cast<uint8_t>((length >> 8) & 0xFF),
      static_cast<uint8_t>((length >> 16) & 0xFF), static_cast<uint8_t>((length >> 24) & 0xFF) };
   CHECK(peer.SendRaw(header));

   Wire::Message msg;
   CHECK(peer.ReadOfType(0, msg, WAIT_MS));
   CHECK(msg.header.IsError());
   Wire::ErrorPayload err;
   CHECK(Wire::Decode(msg.payload, err));
   CHECK_MSG(err.code == Wire::ERR_MALFORMED, "code " + std::to_string(err.code));
   CHECK_MSG(peer.WaitForClose(WAIT_MS), "an oversized length must close the session");

   worker.Stop();
}

// Without a usable token there is nothing to say, so the worker hangs up
// instead of sending a Hello the daemon would have to refuse.
void TestMissingToken()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;

   ScorbitTest::TestPeer peer(dir);
   CHECK(peer.Listening());
   if (!peer.Listening())
      return;
   peer.RemoveToken();

   FakeSource source;
   source.Set(MakeDeclare("", 0, 0, 0, 0), { });

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();

   CHECK(peer.Accept(WAIT_MS));
   Wire::Message msg;
   CHECK_MSG(!peer.Read(msg, 500), "no Hello may go out without a token");
   CHECK(!worker.Connected());

   // Once the token appears the next attempt gets through.
   peer.WriteToken("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));
   CHECK(hello.token == peer.Token());

   worker.Stop();
}

// A daemon that refuses Hello with only the error bit set is still answering,
// not asking. The worker must read it as a refusal and hang up, never dispatch
// it as a request and answer a refusal with a refusal.
void TestErrorWithOnlyTheErrorBit()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;

   ScorbitTest::TestPeer peer(dir);
   CHECK(peer.Listening());
   if (!peer.Listening())
      return;

   FakeSource source;
   source.Set(MakeDeclare("", 0, 0, 0, 0), { });

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS));

   Wire::ErrorPayload err;
   err.code = Wire::ERR_BUSY;
   err.reason = "another instance is already connected";
   CHECK(peer.Send(Wire::TYPE_HELLO, Wire::FLAG_ERROR, msg.header.seq, Wire::Encode(err)));

   CHECK_MSG(peer.WaitForClose(WAIT_MS), "a refused Hello must close the session with nothing sent back");
   CHECK(!worker.Connected());

   // And it keeps trying, because busy is a condition that can clear.
   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));

   worker.Stop();
}

}

int main()
{
   TestSession();
   TestOversizedLength();
   TestMissingToken();
   TestErrorWithOnlyTheErrorBit();
   return ScorbitTest::Summary("socket_worker_test");
}
