// license:GPLv3+

// Drives a real SocketWorker against the scripted peer in TestPeer.h: the whole
// session from connect to Bye, without Visual Pinball and without a daemon.

#include "SocketWorker.h"

#include "Check.h"
#include "TestPeer.h"
#include "WireVectors.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace Scorbit;

namespace
{

constexpr int WAIT_MS = 5000;

class FakeSource final : public ISessionSource
{
public:
   std::unique_ptr<DeclareSnapshot> TakeDeclare() override { return m_declares.Take(); }

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

   // Posts the declaration as the API thread would, and publishes the frame.
   void Set(const DeclareSnapshot& declare, const FrameSnapshot& frame)
   {
      {
         std::lock_guard lock(m_mutex);
         m_frame = frame;
      }
      m_declares.Post(declare);
   }

   void SetSession(const SessionSnapshot& session)
   {
      std::lock_guard lock(m_mutex);
      m_session = session;
   }

private:
   std::mutex m_mutex;
   Latch<DeclareSnapshot> m_declares;
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

   // --- A replacement with identical geometry ----------------------------
   // The tap bumps its generation on every source change, and a replaced
   // source may restart its frame counter at a value the daemon has already
   // seen. Geometry, depth and frame id can therefore all match across the
   // replacement, leaving the generation as the only thing that differs. An
   // unchanged reply here would hand the daemon the previous source's pixels.
   {
      FrameSnapshot frame;
      Fill(frame, 1, 3, 192, 64, 16);
      for (size_t i = 0; i < frame.pixels.size(); i++)
         frame.pixels[i] = static_cast<uint8_t>((i * 7 + 5) % 16);
      source.Set(MakeDeclare("ij_l7", 192, 64, 16, 3), frame);
   }
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare replaced;
   CHECK(Wire::Decode(msg.payload, replaced));
   CHECK_MSG(replaced.sourceGeneration == 3, "a same-geometry replacement must still re-declare");
   CHECK(replaced.width == 192 && replaced.height == 64 && replaced.shades == 16);
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   // The daemon asks with exactly the frame id it last saw, at the generation
   // it last saw. Only the generation says the pixels are not the same ones.
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 3100, Wire::Encode(Wire::FrameRequest { 1, 2 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK_MSG(reply.hasPixels == 1, "a restarted frame id at a new generation must resend pixels");
   CHECK(reply.frameId == 1 && reply.sourceGeneration == 3);
   CHECK(reply.pixels.size() == 192u * 64u);
   CHECK_MSG(reply.pixels[0] == 5 && reply.pixels[1] == 12, "the new source's pixels, not the old ones");

   // And once the daemon has caught up, the same request is unchanged again.
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 3101, Wire::Encode(Wire::FrameRequest { 1, 3 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.hasPixels == 0 && reply.frameId == 1 && reply.sourceGeneration == 3);

   // --- No source is a steady state, not an error ------------------------
   source.Set(MakeDeclare("", 0, 0, 0, 4), FrameSnapshot { false, 0, 4, 0, 0, 0, { } });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare none;
   CHECK(Wire::Decode(msg.payload, none));
   CHECK(none.shades == 0 && none.identifyFormat.empty() && none.sourceGeneration == 4);
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 4000, Wire::Encode(Wire::FrameRequest { 0, 0 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.frameId == 0 && reply.hasPixels == 0 && reply.sourceGeneration == 4);
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

// The reserved since_frame_id means the daemon holds no frame, and it has to
// work even when a real frame carries that same id. That collision is the
// whole reason for the sentinel: a daemon holding nothing used to ask with a
// generation it believed impossible, and no value is.
void TestNoFrameHeldSentinel()
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
   {
      // The one frame id that collides with the sentinel.
      FrameSnapshot frame;
      Fill(frame, Wire::SINCE_FRAME_NONE, 1, 128, 32, 4);
      source.Set(MakeDeclare("ij_l7", 128, 32, 4, 1), frame);
   }

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));

   // Frame id and generation both match what the plugin holds, and the answer
   // must still be pixels, because that frame id is the reserved one.
   Wire::Message msg;
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 6000, Wire::Encode(Wire::FrameRequest { Wire::SINCE_FRAME_NONE, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   Wire::FrameReply reply;
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK_MSG(reply.hasPixels == 1, "the reserved frame id must always draw pixels");
   CHECK(reply.frameId == Wire::SINCE_FRAME_NONE && reply.sourceGeneration == 1);
   CHECK(reply.pixels.size() == 128u * 32u);

   // An ordinary frame id still takes the unchanged path, so the sentinel has
   // not quietly disabled the optimisation.
   {
      FrameSnapshot frame;
      Fill(frame, 7, 1, 128, 32, 4);
      source.Set(MakeDeclare("ij_l7", 128, 32, 4, 1), frame);
   }
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 6001, Wire::Encode(Wire::FrameRequest { 7, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK_MSG(reply.hasPixels == 0, "an ordinary matching frame id is still unchanged");
   CHECK(reply.frameId == 7 && reply.sourceGeneration == 1);

   // And the sentinel overrides that same state.
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 6002, Wire::Encode(Wire::FrameRequest { Wire::SINCE_FRAME_NONE, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.hasPixels == 1 && reply.frameId == 7);
   CHECK(reply.pixels.size() == 128u * 32u);

   // --- The swap gap, where the sentinel cannot be obeyed literally -------
   // The generation has moved and the new source has not produced a frame, so
   // no pixels exist to send. The daemon sends the sentinel after every source
   // change, so this window is reached on an ordinary table swap rather than
   // in some corner. The no-source reply wins over the sentinel here.
   source.Set(MakeDeclare("ij_l7", 0, 0, 0, 2), FrameSnapshot { false, 0, 2, 0, 0, 0, { } });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare gap;
   CHECK(Wire::Decode(msg.payload, gap));
   CHECK(gap.shades == 0 && gap.sourceGeneration == 2);
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 6003, Wire::Encode(Wire::FrameRequest { Wire::SINCE_FRAME_NONE, 1 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK_MSG(reply.hasPixels == 0, "the sentinel cannot conjure pixels that do not exist");
   CHECK_MSG(reply.width == 0 && reply.height == 0 && reply.shades == 0,
      "zero geometry is what tells the daemon nothing is available yet, not unchanged");
   CHECK(reply.frameId == 0 && reply.sourceGeneration == 2);
   CHECK(reply.pixels.empty());

   // And once the new source renders, the same request delivers again.
   {
      FrameSnapshot frame;
      Fill(frame, 1, 2, 128, 32, 4);
      source.Set(MakeDeclare("ij_l7", 128, 32, 4, 2), frame);
   }
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.Send(Wire::TYPE_FRAME, 0, 6004, Wire::Encode(Wire::FrameRequest { Wire::SINCE_FRAME_NONE, 2 })));
   CHECK(peer.ReadOfType(Wire::TYPE_FRAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, reply));
   CHECK(reply.hasPixels == 1 && reply.sourceGeneration == 2);
   CHECK(reply.width == 128 && reply.height == 32 && reply.shades == 4);
   CHECK(reply.pixels.size() == 128u * 32u);

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


// SB-4600 deliverable 4: bytes that actually crossed, not bytes we encoded ourselves.
//
// Every other test in this file has the peer build its replies with Wire::Encode — the
// plugin's own encoder — so a shared misreading of design.md would satisfy both sides
// and pass. The vectors in WireVectors.h came off the wire between this plugin and the
// real daemon, so agreeing with them is interoperability rather than self consistency.
uint32_t LengthPrefix(const std::vector<uint8_t>& raw)
{
   return static_cast<uint32_t>(raw[0]) | static_cast<uint32_t>(raw[1]) << 8
        | static_cast<uint32_t>(raw[2]) << 16 | static_cast<uint32_t>(raw[3]) << 24;
}

bool OpenVector(const char* name, const std::string& hex, Wire::Message& out)
{
   const std::vector<uint8_t> raw = ScorbitTest::Unhex(hex);
   CHECK_MSG(raw.size() > Wire::LENGTH_BYTES, name);
   if (raw.size() <= Wire::LENGTH_BYTES)
      return false;
   CHECK_MSG(LengthPrefix(raw) == raw.size() - Wire::LENGTH_BYTES, name);
   const bool ok = Wire::DecodeBody(raw.data() + Wire::LENGTH_BYTES,
                                    raw.size() - Wire::LENGTH_BYTES, out);
   CHECK_MSG(ok, name);
   return ok;
}

// The SB-4600 populated Declare is withheld from this public repo: its tablePath names a
// home directory and an email address. It lives beside the corpus and is loaded only
// when SCORBIT_WIRE_VECTORS points at that file. Absent, the checks that need it skip.
std::string PrivateVector(const char* name)
{
   const char* path = getenv("SCORBIT_WIRE_VECTORS");
   if (path == nullptr || path[0] == '\0')
      return { };
   std::ifstream in(path);
   // Line based, not token based: the file carries '#' comments, and reading it as a
   // token stream lets an odd comment word shift every following pair by one.
   std::string line;
   while (std::getline(in, line))
   {
      if (line.empty() || line[0] == '#')
         continue;
      const size_t sep = line.find(' ');
      if (sep == std::string::npos)
         continue;
      if (line.compare(0, sep, name) == 0)
         return line.substr(sep + 1);
   }
   return { };
}

void TestCapturedWireBytes()
{
   Wire::Message msg;

   // --- Every public vector decodes, with the type it was labelled with -------
   const struct { const char* name; const std::string* hex; uint16_t type; } vectors[] = {
      { "HELLO_ACK", &Vectors::HELLO_ACK, Wire::TYPE_HELLO_ACK },
      { "DECLARE_NO_SOURCE", &Vectors::DECLARE_NO_SOURCE, Wire::TYPE_DECLARE },
      { "FRAME_REQUEST_SENTINEL", &Vectors::FRAME_REQUEST_SENTINEL, Wire::TYPE_FRAME },
      { "FRAME_REQUEST", &Vectors::FRAME_REQUEST, Wire::TYPE_FRAME },
      { "FRAME_REPLY_PIXELS", &Vectors::FRAME_REPLY_PIXELS, Wire::TYPE_FRAME },
      { "FRAME_REPLY_UNCHANGED", &Vectors::FRAME_REPLY_UNCHANGED, Wire::TYPE_FRAME },
      { "FRAME_REPLY_NO_SOURCE", &Vectors::FRAME_REPLY_NO_SOURCE, Wire::TYPE_FRAME },
   };
   for (const auto& v : vectors)
      if (OpenVector(v.name, *v.hex, msg))
         CHECK_MSG(msg.header.type == v.type, v.name);

   // --- The sentinel is the real constant, not a literal that happens to match -
   CHECK(OpenVector("FRAME_REQUEST_SENTINEL", Vectors::FRAME_REQUEST_SENTINEL, msg));
   Wire::FrameRequest sentinel { };
   CHECK(Wire::Decode(msg.payload, sentinel));
   CHECK(sentinel.sinceFrameId == Wire::SINCE_FRAME_NONE);

   CHECK(OpenVector("FRAME_REQUEST", Vectors::FRAME_REQUEST, msg));
   Wire::FrameRequest normal { };
   CHECK(Wire::Decode(msg.payload, normal));
   CHECK(normal.sinceFrameId == 0 && normal.sinceGeneration == 1);

   // --- The three reply shapes stay distinct ---------------------------------
   CHECK(OpenVector("FRAME_REPLY_PIXELS", Vectors::FRAME_REPLY_PIXELS, msg));
   Wire::FrameReply pixels { };
   CHECK(Wire::Decode(msg.payload, pixels));
   CHECK(pixels.width == 128 && pixels.height == 32 && pixels.shades == 4);
   CHECK(pixels.hasPixels == 1);
   CHECK(pixels.pixels.size() == 128u * 32u);
   bool inRange = true;
   for (const uint8_t p : pixels.pixels)
      if (p > 3)
         inRange = false;
   CHECK_MSG(inRange, "every shade index within [0,3] for a 4 shade source");

   CHECK(OpenVector("FRAME_REPLY_UNCHANGED", Vectors::FRAME_REPLY_UNCHANGED, msg));
   Wire::FrameReply unchanged { };
   CHECK(Wire::Decode(msg.payload, unchanged));
   CHECK(unchanged.hasPixels == 0);
   CHECK_MSG(unchanged.width == 128 && unchanged.shades == 4,
             "unchanged keeps real geometry, unlike the no source reply");

   CHECK(OpenVector("FRAME_REPLY_NO_SOURCE", Vectors::FRAME_REPLY_NO_SOURCE, msg));
   Wire::FrameReply noSource { };
   CHECK(Wire::Decode(msg.payload, noSource));
   CHECK(noSource.hasPixels == 0);
   CHECK_MSG(noSource.width == 0 && noSource.height == 0 && noSource.shades == 0,
             "no source is zero geometry with shades 0, which design.md defines as "
             "nothing available yet");

   // --- Re encoding a decoded capture must reproduce it byte for byte ---------
   // The assertion that catches encoder drift: this build's output compared against
   // bytes the real daemon actually accepted, not against our own idea of them.
   CHECK(OpenVector("FRAME_REPLY_PIXELS", Vectors::FRAME_REPLY_PIXELS, msg));
   CHECK(Wire::Decode(msg.payload, pixels));
   CHECK_MSG(Wire::Encode(pixels) == msg.payload, "FrameReply re encode is byte identical");

   CHECK(OpenVector("DECLARE_NO_SOURCE", Vectors::DECLARE_NO_SOURCE, msg));
   Wire::Declare opening { };
   CHECK(Wire::Decode(msg.payload, opening));
   CHECK_MSG(Wire::Encode(opening) == msg.payload, "Declare re encode is byte identical");
   CHECK_MSG(opening.romId.empty() && opening.width == 0 && opening.sourceGeneration == 0,
             "the opening declare precedes any ROM");

   CHECK(OpenVector("HELLO_ACK", Vectors::HELLO_ACK, msg));
   Wire::HelloAck ack { };
   CHECK(Wire::Decode(msg.payload, ack));
   CHECK_MSG(Wire::Encode(ack) == msg.payload, "HelloAck re encode is byte identical");
   CHECK(ack.protoMajor == Wire::PROTO_MAJOR && ack.protoMinor == Wire::PROTO_MINOR);

   CHECK(OpenVector("HELLO", Vectors::HELLO, msg));
   CHECK(msg.header.type == Wire::TYPE_HELLO && msg.header.seq == 1);
   Wire::Hello hello { };
   CHECK(Wire::Decode(msg.payload, hello));
   CHECK_MSG(Wire::Encode(hello) == msg.payload, "Hello re encode is byte identical");
   CHECK(hello.protoMajor == Wire::PROTO_MAJOR && hello.protoMinor == Wire::PROTO_MINOR);
   CHECK(hello.vpxRevision == "5589");
   CHECK(hello.token == std::string(64, '0'));
   CHECK_MSG(hello.capabilities == (Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_MEMORY_READ
                                    | Wire::CAP_MEMORY_WRITE),
             "a slice 2 plugin advertises frames, memory read and memory write");

   CHECK(OpenVector("DECLARE_METEOR", Vectors::DECLARE_METEOR, msg));
   CHECK(msg.header.type == Wire::TYPE_DECLARE);
   Wire::Declare meteor { };
   CHECK(Wire::Decode(msg.payload, meteor));
   CHECK_MSG(Wire::Encode(meteor) == msg.payload, "Meteor Declare re encode is byte identical");
   CHECK(meteor.romId == "meteor");
   CHECK_MSG(meteor.tablePath == "Meteor (Stern 1979).vpx", "the table by file name, never a path");
   CHECK_MSG(meteor.width == 0 && meteor.height == 0 && meteor.shades == 0
                && meteor.identifyFormat.empty(),
             "segment displays declare no conforming source");

   // --- The withheld DMD Declare, only when the corpus file is pointed at -----
   const std::string declareHex = PrivateVector("DECLARE");
   if (declareHex.empty())
   {
      fprintf(stderr, "skip: the populated DMD Declare needs SCORBIT_WIRE_VECTORS "
                      "(it carries a personal path, so it is not in this repo)\n");
      return;
   }

   CHECK(OpenVector("DECLARE", declareHex, msg));
   Wire::Declare declare { };
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK_MSG(Wire::Encode(declare) == msg.payload, "Declare re encode is byte identical");
   CHECK(declare.romId == "tom_13");
   CHECK(declare.width == 128 && declare.height == 32 && declare.shades == 4);
   CHECK_MSG(declare.identifyFormat == "BITPLANE2",
             "names the source format; frame payloads are unpacked one byte per pixel");
}

// The handshake driven by the daemon's real bytes: the peer replays the captured
// HelloAck verbatim instead of encoding one, and the worker must accept it and go on
// to Declare. Nothing the plugin produced takes part in the reply.
void TestHandshakeAgainstCapturedAck()
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
   FrameSnapshot frame;
   Fill(frame, 10, 1, 128, 32, 4);
   source.Set(MakeDeclare("tom_13", 128, 32, 4, 1), frame);
   source.SetSession(SessionSnapshot { 1, true });

   SocketWorker worker(source, MakeConfig(peer), Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS));

   // The captured ack carries seq 1, which is the seq the worker's first Hello uses.
   // Asserted rather than assumed: if the worker ever renumbers, this says so plainly
   // instead of the replay silently failing to match.
   CHECK_MSG(msg.header.seq == 1, "worker's first Hello is seq 1, so the capture replays as is");

   CHECK_MSG(peer.SendRaw(ScorbitTest::Unhex(Vectors::HELLO_ACK)),
             "replay the daemon's own HelloAck bytes");

   CHECK_MSG(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS),
             "worker accepted the captured HelloAck and declared");
   Wire::Declare declare { };
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK(declare.romId == "tom_13");

   worker.Stop();
}

}

// A flat 64 KiB main CPU memory, served by a thread standing in for VPX's API
// thread, which calls MemoryBridge::Service once per "frame".
class ServedMemory
{
public:
   explicit ServedMemory(MemoryBridge& bridge, bool serve = true, bool canWrite = true)
      : m_bytes(0x10000)
   {
      for (size_t i = 0; i < m_bytes.size(); i++)
         m_bytes[i] = static_cast<uint8_t>(i ^ 0x5a);
      bridge.SetGameRunning(true); // as SetRomId would, before any frame
      if (!serve)
         return;
      m_thread = std::thread([this, &bridge, canWrite]
         {
            const MemoryBridge::ReadFn read = [this](uint32_t a, uint8_t* out, uint32_t n)
            {
               uint32_t i = 0;
               for (; i < n && a + i < m_bytes.size(); i++)
                  out[i] = m_bytes[a + i];
               return i;
            };
            // Like PinMAME with pinmame #634, or without it (applies nothing).
            const MemoryBridge::WriteFn write = [this, canWrite](const std::vector<Wire::WriteOp>& ops)
            {
               if (!canWrite)
                  return MemoryBridge::NO_WRITE_HANDLER;
               for (const Wire::WriteOp& op : ops)
                  for (size_t i = 0; i < op.bytes.size() && op.address + i < m_bytes.size(); i++)
                     m_bytes[op.address + i] = op.bytes[i];
               return static_cast<uint32_t>(ops.size());
            };
            uint64_t frame = 0;
            while (m_running)
            {
               bridge.Service(read, ++frame, true, true, write);
               std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
         });
   }

   ~ServedMemory()
   {
      m_running = false;
      if (m_thread.joinable())
         m_thread.join();
   }

   uint8_t At(uint32_t address) const { return m_bytes[address]; }

private:
   std::vector<uint8_t> m_bytes;
   std::atomic<bool> m_running { true };
   std::thread m_thread;
};

// Reads the response to seq, or the error it carries.
bool Reply(ScorbitTest::TestPeer& peer, uint16_t type, uint32_t seq, Wire::Message& msg, uint16_t& errorCode)
{
   errorCode = 0;
   if (!peer.ReadOfType(type, msg, WAIT_MS) || msg.header.seq != seq || !msg.header.IsResponse())
      return false;
   if (msg.header.IsError())
   {
      Wire::ErrorPayload err;
      if (!Wire::Decode(msg.payload, err) || err.reason.empty())
         return false; // a refusal must carry a full code and reason
      errorCode = err.code;
   }
   return true;
}

// Slice 2 end to end: advertised, Subscribe echoes, Poll carries the set,
// ReadDirect reads, NvRam and Write are served, and a Declare clears the set.
void TestMemoryReadsOverTheSocket()
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
   MemoryBridge bridge;
   ServedMemory memory(bridge);
   SocketWorker worker(source, MakeConfig(peer), Quiet(), &bridge);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));
   CHECK(hello.capabilities == (Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_MEMORY_READ | Wire::CAP_MEMORY_WRITE));

   Wire::Message msg;
   uint16_t code = 0;

   // Subscribe: echoed exactly, in order.
   const Wire::Subscription want { { { 0x1c93, 4 }, { 0x0100, 8 } } };
   CHECK(peer.Send(Wire::TYPE_SUBSCRIBE, 0, 50, Wire::Encode(want)));
   CHECK(Reply(peer, Wire::TYPE_SUBSCRIBE, 50, msg, code) && code == 0);
   Wire::Subscription echoed;
   CHECK(Wire::Decode(msg.payload, echoed) && echoed == want);

   // Poll: the subscribed set exactly, bytes and all.
   CHECK(peer.Send(Wire::TYPE_POLL, 0, 51, { }));
   CHECK(Reply(peer, Wire::TYPE_POLL, 51, msg, code) && code == 0);
   Wire::PollReply poll;
   CHECK(Wire::Decode(msg.payload, poll));
   CHECK(poll.blocks.size() == 2);
   if (poll.blocks.size() == 2)
   {
      CHECK(poll.blocks[0].address == 0x1c93 && poll.blocks[0].bytes.size() == 4);
      CHECK(poll.blocks[0].bytes[3] == memory.At(0x1c96));
      CHECK(poll.blocks[1].address == 0x0100 && poll.blocks[1].bytes.size() == 8);
   }

   // ReadDirect: a signature-sized window, all of it stable.
   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 52, Wire::Encode(Wire::ReadDirectRequest { 0x8000, 0x8000 })));
   CHECK(Reply(peer, Wire::TYPE_READ_DIRECT, 52, msg, code) && code == 0);
   Wire::ReadDirectReply read;
   CHECK(Wire::Decode(msg.payload, read));
   CHECK(read.address == 0x8000 && read.bytes.size() == 0x8000 && read.unstableOffsets.empty());
   CHECK(read.bytes.size() == 0x8000 && read.bytes[0x7fff] == memory.At(0xffff));

   // Past the end of the address space: short, not refused.
   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 53, Wire::Encode(Wire::ReadDirectRequest { 0xfff0, 0x100 })));
   CHECK(Reply(peer, Wire::TYPE_READ_DIRECT, 53, msg, code) && code == 0);
   CHECK(Wire::Decode(msg.payload, read) && read.bytes.size() == 0x10);

   // Over the limit: too_large, with a reason.
   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 54, Wire::Encode(Wire::ReadDirectRequest { 0, 0x10001 })));
   CHECK(Reply(peer, Wire::TYPE_READ_DIRECT, 54, msg, code) && code == Wire::ERR_TOO_LARGE);

   // NvRam, as the daemon sends it before its first Write: served, with an empty reply.
   CHECK(peer.Send(Wire::TYPE_NVRAM, 0, 55, ScorbitTest::Unhex("00180000" "00080000" "fd3f0000" "b4" "b4")));
   CHECK(Reply(peer, Wire::TYPE_NVRAM, 55, msg, code) && code == 0 && msg.payload.empty());

   // Write, scorbitd's golden request: the unlock and a two-byte payload, both applied, and the
   // payload is in the next Poll of the subscribed range.
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 57, ScorbitTest::Unhex("0200" "fd3f0000" "0100" "b4" "931c0000" "0200" "0801")));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 57, msg, code) && code == 0);
   Wire::WriteReply written;
   CHECK(Wire::Decode(msg.payload, written) && written.applied == 2 && written.frame != 0);
   CHECK(peer.Send(Wire::TYPE_POLL, 0, 58, { }));
   CHECK(Reply(peer, Wire::TYPE_POLL, 58, msg, code) && code == 0);
   CHECK(Wire::Decode(msg.payload, poll) && poll.blocks.size() == 2);
   if (poll.blocks.size() == 2)
      CHECK(poll.blocks[0].bytes[0] == 0x08 && poll.blocks[0].bytes[1] == 0x01);

   // Seventeen ops is past the limit; an empty Write names nothing to write.
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 59, Wire::Encode(Wire::WriteRequest {
      std::vector<Wire::WriteOp>(Wire::MAX_WRITE_OPS + 1, { 0x100, { 1 } }) })));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 59, msg, code) && code == Wire::ERR_TOO_LARGE);
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 60, Wire::Encode(Wire::WriteRequest { })));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 60, msg, code) && code == Wire::ERR_OUT_OF_RANGE);

   // A new Declare clears the set on both sides.
   source.Set(MakeDeclare("tom_13", 128, 32, 4, 2), { });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.Send(Wire::TYPE_POLL, 0, 56, { }));
   CHECK(Reply(peer, Wire::TYPE_POLL, 56, msg, code) && code == 0);
   CHECK(Wire::Decode(msg.payload, poll));
   CHECK_MSG(poll.blocks.empty(), "the previous ROM's subscription survived a Declare");

   worker.Stop();
}

// Stock VPX: PinMAME has no write handler. Writes answer unsupported_operation, at once and
// every time, while reads go on working.
void TestWriteOnAHostWithoutWriteSupport()
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
   MemoryBridge bridge;
   ServedMemory memory(bridge, true, false);
   SocketWorker worker(source, MakeConfig(peer), Quiet(), &bridge);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));

   Wire::Message msg;
   uint16_t code = 0;
   const std::vector<uint8_t> write = ScorbitTest::Unhex("0100" "931c0000" "0100" "08");
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 70, write));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 70, msg, code) && code == Wire::ERR_UNSUPPORTED_OPERATION);

   const auto t0 = std::chrono::steady_clock::now();
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 71, write));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 71, msg, code) && code == Wire::ERR_UNSUPPORTED_OPERATION);
   const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
   CHECK_MSG(ms < 50, "a remembered unsupported write took " + std::to_string(ms) + " ms");

   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 72, Wire::Encode(Wire::ReadDirectRequest { 0x1c93, 1 })));
   CHECK(Reply(peer, Wire::TYPE_READ_DIRECT, 72, msg, code) && code == 0);
   Wire::ReadDirectReply read;
   CHECK(Wire::Decode(msg.payload, read) && read.bytes.size() == 1 && read.bytes[0] == memory.At(0x1c93));

   worker.Stop();
}

// Nothing services memory (VPX stopped rendering): the plugin answers busy within
// the daemon's 100 ms, and unload with a read in flight returns promptly.
void TestMemoryReadWithoutAnApiThread()
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
   MemoryBridge bridge;
   // A game is running, but no frame renders, so nothing ever calls Service.
   bridge.SetGameRunning(true);
   SocketWorker worker(source, MakeConfig(peer), Quiet(), &bridge);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   Wire::Declare declare;
   CHECK(Handshake(peer, hello, declare));

   Wire::Message msg;
   uint16_t code = 0;
   const auto t0 = std::chrono::steady_clock::now();
   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 60, Wire::Encode(Wire::ReadDirectRequest { 0x0100, 4 })));
   CHECK(Reply(peer, Wire::TYPE_READ_DIRECT, 60, msg, code) && code == Wire::ERR_BUSY);
   const auto busyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
   CHECK_MSG(busyMs < 100, "busy arrived after " + std::to_string(busyMs) + " ms, past the daemon's timeout");

   // A read in flight when unload arrives.
   CHECK(peer.Send(Wire::TYPE_READ_DIRECT, 0, 61, Wire::Encode(Wire::ReadDirectRequest { 0x0100, 4 })));
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   const auto t1 = std::chrono::steady_clock::now();
   worker.Stop();
   const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
   CHECK_MSG(stopMs < 500, "Stop() with a read in flight took " + std::to_string(stopMs) + " ms");
}

// Collects the worker's log so a test can wait for a line and read it back.
class LogCapture final
{
public:
   SocketWorker::LogFn Fn()
   {
      return [this](int, const std::string& message)
      {
         std::lock_guard lock(m_mutex);
         m_lines.push_back(message);
      };
   }

   // The first line containing needle, or empty after timeoutMs.
   std::string WaitFor(const std::string& needle, int timeoutMs)
   {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
      while (std::chrono::steady_clock::now() < deadline)
      {
         {
            std::lock_guard lock(m_mutex);
            for (const std::string& line : m_lines)
               if (line.find(needle) != std::string::npos)
                  return line;
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return { };
   }

   std::string WaitForAny(const std::vector<std::string>& needles, int timeoutMs)
   {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
      while (std::chrono::steady_clock::now() < deadline)
      {
         for (const std::string& needle : needles)
            if (Any(needle))
               return needle;
         std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return { };
   }

   bool Any(const std::string& needle)
   {
      std::lock_guard lock(m_mutex);
      for (const std::string& line : m_lines)
         if (line.find(needle) != std::string::npos)
            return true;
      return false;
   }

private:
   std::mutex m_mutex;
   std::vector<std::string> m_lines;
};

SocketWorkerConfig MakeConfigAt(const std::string& dir)
{
   SocketWorkerConfig cfg;
   cfg.socketPath = dir + "/vpx.sock";
   cfg.tokenPath = dir + "/vpx.token";
   cfg.pluginVersion = "0.1.0";
   cfg.pluginApiCommit = "0bc9838ed5f1bbac869efdfb6e785b829a541d3f";
   return cfg;
}

int BindUnix(const std::string& path)
{
   const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   sockaddr_un addr { };
   addr.sun_family = AF_UNIX;
   memcpy(addr.sun_path, path.c_str(), path.size());
   if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   return fd;
}

// Nothing at the path: the one case where "the daemon is not running" is true.
void TestConnectNoSuchPath()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;

   FakeSource source;
   LogCapture log;
   SocketWorker worker(source, MakeConfigAt(dir), log.Fn());
   worker.Start();
   CHECK_MSG(!log.WaitFor("nothing at " + dir + "/vpx.sock", WAIT_MS).empty(), "missing path not reported as such");
   worker.Stop();
   CHECK(!log.Any("refused") && !log.Any("not accepting"));
   rmdir(dir.c_str());
}

// A socket file nobody listens on, as a daemon that exited leaves behind.
void TestConnectRefused()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;
   const std::string path = dir + "/vpx.sock";
   const int bound = BindUnix(path);
   CHECK(bound >= 0);

   FakeSource source;
   LogCapture log;
   SocketWorker worker(source, MakeConfigAt(dir), log.Fn());
   worker.Start();
   const std::string line = log.WaitFor("refused the connection", WAIT_MS);
   CHECK_MSG(!line.empty(), "stale socket not reported as refused");
   CHECK_MSG(line.find("error ") != std::string::npos, "no error code in '" + line + "'");
   worker.Stop();
   CHECK(!log.Any("nothing at"));

   if (bound >= 0)
      close(bound);
   unlink(path.c_str());
   rmdir(dir.c_str());
}

// SB-4692's daemon: listening, never accepting, queue already full. A blocking
// connect waits forever for this on Linux, so Stop() would never return (SB-5083).
void TestConnectToListenerThatNeverAccepts()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;
   const std::string path = dir + "/vpx.sock";
   const int listener = BindUnix(path);
   CHECK(listener >= 0 && listen(listener, 1) == 0);

   // Fill the queue the way earlier unaccepted connections would.
   sockaddr_un addr { };
   addr.sun_family = AF_UNIX;
   memcpy(addr.sun_path, path.c_str(), path.size());
   std::vector<int> queued;
   for (int i = 0; i < 64; i++)
   {
      const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
         break;
      fcntl(fd, F_SETFL, O_NONBLOCK);
      if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
      {
         close(fd);
         break;
      }
      queued.push_back(fd);
   }

   FakeSource source;
   LogCapture log;
   SocketWorker worker(source, MakeConfigAt(dir), log.Fn());
   worker.Start();
   // Linux reports a full queue as "not accepting"; macOS refuses it.
   const std::string line = log.WaitForAny({ "not accepting", "refused the connection" }, WAIT_MS);
   const bool reported = !line.empty();
   CHECK_MSG(reported, "a listener that never accepts was not reported");

   const auto before = std::chrono::steady_clock::now();
   worker.Stop();
   const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
   CHECK_MSG(stopMs < 1500, "Stop() took " + std::to_string(stopMs) + " ms");
   CHECK(!log.Any("nothing at"));

   for (const int fd : queued)
      close(fd);
   if (listener >= 0)
      close(listener);
   unlink(path.c_str());
   rmdir(dir.c_str());
}

// Records what the worker hands over, with DmdOverlay's upload rules.
class FakeOverlay final : public IOverlayTarget
{
public:
   bool OverlaySurface() override { return m_surface; }

   Overlay::State::Upload OverlayUpload(const uint8_t* payload, size_t size) override
   {
      std::lock_guard lock(m_mutex);
      const Overlay::State::Upload result = Overlay::State::Check(payload, size);
      if (result == Overlay::State::Upload::Stored)
         m_uploads.emplace_back(payload, payload + size);
      return result;
   }

   void OverlayControl(bool enable, uint32_t durationMs) override
   {
      std::lock_guard lock(m_mutex);
      m_controls.emplace_back(enable, durationMs);
   }

   void OverlayClear() override { m_clears++; }

   std::atomic<bool> m_surface { true };
   std::atomic<int> m_clears { 0 };
   std::mutex m_mutex;
   std::vector<std::vector<uint8_t>> m_uploads;
   std::vector<std::pair<bool, uint32_t>> m_controls;
};

// The latch's latest record, waiting for one that passes the test.
template <typename Pred>
bool AwaitStatus(StatusLatch& latch, Pred pred, StatusRecord& out)
{
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WAIT_MS);
   while (std::chrono::steady_clock::now() < deadline)
   {
      if (std::unique_ptr<StatusRecord> r = latch.Take())
      {
         out = *r;
         if (pred(out))
            return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   return false;
}

uint16_t ErrorCode(const Wire::Message& msg)
{
   Wire::ErrorPayload err;
   return msg.header.IsError() && Wire::Decode(msg.payload, err) ? err.code : 0;
}

// Slice 4 end to end: Overlay and Status before and after the Declare, the golden
// requests, the three error codes, and what session end leaves behind.
void TestOverlayAndStatusOverTheSocket()
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
   {
      FrameSnapshot frame;
      Fill(frame, 1, 1, 128, 32, 4);
      source.Set(MakeDeclare("tom_13", 128, 32, 4, 1), frame);
   }
   FakeOverlay overlay;
   StatusLatch latch;
   SocketWorker worker(source, MakeConfig(peer), Quiet(), nullptr, &overlay, &latch);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   // --- Hello advertises overlay; nothing is served before the Declare ---
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS));
   Wire::Hello hello;
   CHECK(Wire::Decode(msg.payload, hello));
   CHECK_MSG(hello.capabilities == (Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_OVERLAY), "caps " + std::to_string(hello.capabilities));
   Wire::HelloAck ack;
   ack.protoMajor = Wire::PROTO_MAJOR;
   CHECK(peer.Send(Wire::TYPE_HELLO_ACK, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(ack)));

   StatusRecord record;
   CHECK_MSG(AwaitStatus(latch, [](const StatusRecord& r) { return r.inSession; }, record), "HelloAck starts a session");
   CHECK(!record.haveStatus);

   // The worker is waiting on its Declare's answer; requests that overtake it are served.
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   const uint32_t declareSeq = msg.header.seq;
   CHECK(peer.SendRaw(ScorbitTest::Unhex("0c000000" "2000" "0000" "08000000" "02" "01" "05dc")));
   CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
   CHECK_MSG(ErrorCode(msg) == Wire::ERR_NOT_DECLARED, "Overlay before the Declare");
   CHECK(peer.Send(Wire::TYPE_STATUS, 0, 9, Wire::Encode(Wire::Status { })));
   CHECK(peer.ReadOfType(Wire::TYPE_STATUS, msg, WAIT_MS));
   CHECK_MSG(ErrorCode(msg) == Wire::ERR_NOT_DECLARED, "Status before the Declare");
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, declareSeq, { }));

   // --- The golden requests, verbatim ------------------------------------
   std::string upload = "6f000000" "2000" "0000" "07000000" "01" "0a" "0a";
   for (int i = 0; i < 10; i++)
      upload += "80";
   for (int i = 0; i < 90; i++)
      upload += "0f";
   CHECK(peer.SendRaw(ScorbitTest::Unhex(upload)));
   CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
   CHECK_MSG(ScorbitTest::Hex(Wire::EncodeMessage(msg.header.type, msg.header.flags, msg.header.seq, msg.payload))
      == "08000000" "2000" "0100" "07000000", "upload acknowledgement");

   const char* controls[] = {
      "0c000000" "2000" "0000" "08000000" "02" "01" "05dc",
      "0c000000" "2000" "0000" "09000000" "02" "01" "0000",
      "0c000000" "2000" "0000" "0a000000" "02" "00" "0000",
      // Reserved control bits are ignored: bit 0 clear is a hide whatever else is set.
      "0c000000" "2000" "0000" "0b000000" "02" "fe" "0100",
   };
   for (const char* frame : controls)
   {
      CHECK(peer.SendRaw(ScorbitTest::Unhex(frame)));
      CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
      CHECK(msg.header.IsResponse() && !msg.header.IsError() && msg.payload.empty());
   }
   {
      std::lock_guard lock(overlay.m_mutex);
      CHECK(overlay.m_uploads.size() == 1 && overlay.m_uploads[0].size() == 102);
      CHECK(overlay.m_uploads.size() == 1 && overlay.m_uploads[0][2] == 0x80 && overlay.m_uploads[0][101] == 0x0f);
      CHECK(overlay.m_controls.size() == 4);
      CHECK(overlay.m_controls.size() == 4 && overlay.m_controls[0] == std::make_pair(true, 1500u));
      CHECK(overlay.m_controls.size() == 4 && overlay.m_controls[1] == std::make_pair(true, 0u));
      CHECK(overlay.m_controls.size() == 4 && !overlay.m_controls[2].first && !overlay.m_controls[3].first);
   }

   // --- Out of bounds is acknowledged and ignored ------------------------
   {
      std::vector<uint8_t> small { Wire::OVERLAY_OP_UPLOAD, 9, 9 };
      small.resize(3 + 81, 0x01);
      CHECK(peer.Send(Wire::TYPE_OVERLAY, 0, 30, small));
      CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
      CHECK_MSG(msg.header.IsResponse() && !msg.header.IsError(), "an out of range upload is still acknowledged");
      std::lock_guard lock(overlay.m_mutex);
      CHECK(overlay.m_uploads.size() == 1);
   }

   // --- Malformed is answered, and the session survives -----------------
   const std::vector<std::vector<uint8_t>> malformed = {
      { },                                          // no op
      { 3, 1, 0, 0 },                               // unknown op
      { Wire::OVERLAY_OP_CONTROL, 1, 0x05 },         // control short of its duration
      { Wire::OVERLAY_OP_UPLOAD, 10, 10, 0, 0, 0 },  // fewer pixels than 10x10
   };
   uint32_t seq = 40;
   for (const std::vector<uint8_t>& payload : malformed)
   {
      CHECK(peer.Send(Wire::TYPE_OVERLAY, 0, seq, payload));
      CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
      CHECK_MSG(ErrorCode(msg) == Wire::ERR_MALFORMED && msg.header.seq == seq, "payload of " + std::to_string(payload.size()));
      seq++;
   }
   {
      const Wire::Status tooLong { 0, 0, std::string(Wire::MAX_SUPPORT_TEXT_BYTES + 1, 'x') };
      CHECK(peer.Send(Wire::TYPE_STATUS, 0, seq, Wire::Encode(tooLong)));
      CHECK(peer.ReadOfType(Wire::TYPE_STATUS, msg, WAIT_MS));
      CHECK_MSG(ErrorCode(msg) == Wire::ERR_MALFORMED, "support_text over 256 bytes");
   }
   CHECK(peer.Send(Wire::TYPE_PING, 0, 60, { }));
   CHECK_MSG(peer.ReadOfType(Wire::TYPE_PONG, msg, WAIT_MS), "the session survives a malformed Overlay or Status");

   // --- Status, the golden request ---------------------------------------
   CHECK(peer.SendRaw(ScorbitTest::Unhex("13000000" "2100" "0000" "0b000000" "02" "00" "0700" "6e6f2064617461")));
   CHECK(peer.ReadOfType(Wire::TYPE_STATUS, msg, WAIT_MS));
   CHECK_MSG(ScorbitTest::Hex(Wire::EncodeMessage(msg.header.type, msg.header.flags, msg.header.seq, msg.payload))
      == "08000000" "2100" "0100" "0b000000", "status acknowledgement");
   CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return r.haveStatus; }, record));
   CHECK(record.inSession && record.status == (Wire::Status { Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "no data" }));

   // --- No surface: refused with code 6 ----------------------------------
   overlay.m_surface = false;
   CHECK(peer.SendRaw(ScorbitTest::Unhex(upload)));
   CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
   CHECK_MSG(ScorbitTest::Hex(Wire::EncodeMessage(msg.header.type, msg.header.flags, msg.header.seq, msg.payload))
      == "19000000" "2000" "0300" "07000000" "0600" "0d00" "6e6f20444d4420736f75726365", "the golden refusal");
   overlay.m_surface = true;

   // A table with no DMD declares shades 0, and is refused the same way.
   source.Set(MakeDeclare("meteor", 0, 0, 0, 2), FrameSnapshot { false, 0, 2, 0, 0, 0, { } });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.Send(Wire::TYPE_OVERLAY, 0, 70, { Wire::OVERLAY_OP_CONTROL, 1, 0, 0 }));
   CHECK(peer.ReadOfType(Wire::TYPE_OVERLAY, msg, WAIT_MS));
   CHECK_MSG(ErrorCode(msg) == Wire::ERR_UNSUPPORTED_OPERATION, "no DMD declared");

   // --- The surface appearing re-declares with nothing else changed ------
   {
      FrameSnapshot frame;
      Fill(frame, 1, 3, 128, 32, 4);
      DeclareSnapshot d = MakeDeclare("tom_13", 128, 32, 4, 3);
      source.Set(d, frame);
      CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
      d.surfaceGeneration = 1;
      source.Set(d, frame);
      CHECK_MSG(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS), "a surface change re-declares");
      Wire::Declare again;
      CHECK(Wire::Decode(msg.payload, again) && again.sourceGeneration == 3 && again.width == 128);
      CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   }

   // --- Session end clears the overlay and says there is no session ------
   CHECK(overlay.m_clears == 0);
   peer.CloseClient();
   CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return !r.inSession; }, record));
   CHECK_MSG(overlay.m_clears == 1, "clears " + std::to_string(overlay.m_clears.load()));
   CHECK(!record.haveStatus && record.reason.empty());
   worker.Stop();
}

// A refused Hello's reason becomes the no-session support text.
void TestRefusedHelloReason()
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
   StatusLatch latch;
   SocketWorker worker(source, MakeConfig(peer), Quiet(), nullptr, nullptr, &latch);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS));
   Wire::Hello hello;
   CHECK(Wire::Decode(msg.payload, hello));
   CHECK_MSG((hello.capabilities & Wire::CAP_OVERLAY) == 0, "no overlay target, no overlay capability");
   CHECK(peer.Send(Wire::TYPE_HELLO, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, msg.header.seq,
      Wire::Encode(Wire::ErrorPayload { Wire::ERR_BUSY, "another VPX is connected" })));
   StatusRecord record;
   CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return !r.reason.empty(); }, record));
   CHECK_MSG(!record.inSession && record.reason == "busy: another VPX is connected", record.reason);
   worker.Stop();
}

int main()
{
   TestSession();
   TestNoFrameHeldSentinel();
   TestOversizedLength();
   TestMissingToken();
   TestErrorWithOnlyTheErrorBit();
   TestCapturedWireBytes();
   TestHandshakeAgainstCapturedAck();
   TestConnectNoSuchPath();
   TestConnectRefused();
   TestConnectToListenerThatNeverAccepts();
   TestMemoryReadsOverTheSocket();
   TestWriteOnAHostWithoutWriteSupport();
   TestMemoryReadWithoutAnApiThread();
   TestOverlayAndStatusOverTheSocket();
   TestRefusedHelloReason();
   return ScorbitTest::Summary("socket_worker_test");
}
