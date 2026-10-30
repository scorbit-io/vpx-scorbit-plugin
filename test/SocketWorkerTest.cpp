// license:GPLv3+

// Drives a real SocketWorker against the scripted peer in TestPeer.h: the whole
// session from connect to Bye, without Visual Pinball and without a daemon.

#include "SocketWorker.h"

#include "Check.h"
#include "TestPeer.h"
#include "WireVectors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

   void TakeGames(std::deque<GamePost>& out) override
   {
      std::lock_guard lock(m_mutex);
      for (GamePost& post : m_games)
         out.push_back(std::move(post));
      m_games.clear();
   }

   // Commits a batch as the API thread would.
   void Commit(uint64_t epoch, const Wire::GameBatch& batch)
   {
      std::lock_guard lock(m_mutex);
      m_games.push_back({ epoch, batch });
   }

private:
   std::mutex m_mutex;
   std::deque<GamePost> m_games;
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
   ack.daemonVersion = "daemon-test";
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
   CHECK_MSG(pong.writesSupported == 0, "a worker with no memory bridge serves no writes");

   // --- Reserved and unknown types are refused, not ignored --------------
   const uint16_t refused[] = { Wire::TYPE_SUBSCRIBE, Wire::TYPE_POLL, Wire::TYPE_READ_DIRECT,
      Wire::TYPE_WRITE, Wire::TYPE_NVRAM, Wire::TYPE_OVERLAY, Wire::TYPE_STATUS, Wire::TYPE_TOP_SCORES, Wire::TYPE_PLAYERS,
      Wire::TYPE_PLAYER_PICTURE, Wire::TYPE_PRICING, 999 };
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


// Bytes that actually crossed, not bytes we encoded ourselves.
//
// Every other test in this file has the peer build its replies with Wire::Encode — the
// plugin's own encoder — so a shared misreading of the wire protocol definition would satisfy both sides
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

// One recorded vector, a populated Declare, is not in this repository. It is loaded only
// when SCORBIT_WIRE_VECTORS points at a file holding it; absent, the checks that need it skip.
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
             "no source is zero geometry with shades 0, which the protocol defines as "
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
   CHECK_MSG(Wire::Encode(opening, 0) == msg.payload, "Declare re encode is byte identical");
   CHECK_MSG(opening.romId.empty() && opening.width == 0 && opening.sourceGeneration == 0,
             "the opening declare precedes any ROM");

   CHECK(OpenVector("HELLO_ACK", Vectors::HELLO_ACK, msg));
   Wire::HelloAck ack { };
   CHECK(Wire::Decode(msg.payload, ack));
   CHECK_MSG(Wire::Encode(ack) == msg.payload, "HelloAck re encode is byte identical");
   CHECK_MSG(ack.protoMajor == Wire::PROTO_MAJOR && ack.protoMinor == 0, "captured from a 1.0 daemon");

   CHECK(OpenVector("HELLO", Vectors::HELLO, msg));
   CHECK(msg.header.type == Wire::TYPE_HELLO && msg.header.seq == 1);
   Wire::Hello hello { };
   CHECK(Wire::Decode(msg.payload, hello));
   CHECK_MSG(Wire::Encode(hello) == msg.payload, "Hello re encode is byte identical");
   CHECK_MSG(hello.protoMajor == Wire::PROTO_MAJOR && hello.protoMinor == 0, "captured from a 1.0 plugin");
   CHECK(hello.vpxRevision == "5589");
   CHECK(hello.token == std::string(64, '0'));
   CHECK_MSG(hello.capabilities == (Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_MEMORY_READ
                                    | Wire::CAP_MEMORY_WRITE),
             "a slice 2 plugin advertises frames, memory read and memory write");

   CHECK(OpenVector("DECLARE_METEOR", Vectors::DECLARE_METEOR, msg));
   CHECK(msg.header.type == Wire::TYPE_DECLARE);
   Wire::Declare meteor { };
   CHECK(Wire::Decode(msg.payload, meteor));
   CHECK_MSG(Wire::Encode(meteor, 0) == msg.payload, "Meteor Declare re encode is byte identical");
   CHECK(meteor.romId == "meteor");
   CHECK_MSG(meteor.tablePath == "Meteor (Stern 1979).vpx", "the table by file name, never a path");
   CHECK_MSG(meteor.width == 0 && meteor.height == 0 && meteor.shades == 0
                && meteor.identifyFormat.empty(),
             "segment displays declare no conforming source");

   // --- The recorded DMD Declare, only when SCORBIT_WIRE_VECTORS supplies it ---
   const std::string declareHex = PrivateVector("DECLARE");
   if (declareHex.empty())
   {
      fprintf(stderr, "skip: the populated DMD Declare needs SCORBIT_WIRE_VECTORS "
                      "(it is not in this repository)\n");
      return;
   }

   CHECK(OpenVector("DECLARE", declareHex, msg));
   Wire::Declare declare { };
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK_MSG(Wire::Encode(declare, 0) == msg.payload, "Declare re encode is byte identical");
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

   // Write, the daemon's golden request: the unlock and a two-byte payload, both applied, and the
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
   auto pongWrites = [&](uint32_t seq) -> int
   {
      if (!peer.Send(Wire::TYPE_PING, 0, seq, { }) || !peer.ReadOfType(Wire::TYPE_PONG, msg, WAIT_MS))
         return -1;
      Wire::Pong pong;
      return Wire::Decode(msg.payload, pong) ? pong.writesSupported : -1;
   };
   CHECK_MSG(pongWrites(68) == 1, "Pong says writes are served until the host shows otherwise");
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 70, write));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 70, msg, code) && code == Wire::ERR_UNSUPPORTED_OPERATION);

   const auto t0 = std::chrono::steady_clock::now();
   CHECK(peer.Send(Wire::TYPE_WRITE, 0, 71, write));
   CHECK(Reply(peer, Wire::TYPE_WRITE, 71, msg, code) && code == Wire::ERR_UNSUPPORTED_OPERATION);
   const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
   CHECK_MSG(ms < 50, "a remembered unsupported write took " + std::to_string(ms) + " ms");
   CHECK_MSG(pongWrites(69) == 0, "after the host applied no write, Pong says so");

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

// A GameOp with the fields its kind does not use left at zero.
Wire::GameOp Op(uint8_t kind, uint8_t value = 0, int64_t score = 0, int32_t feature = 0, uint32_t seconds = 0,
   const std::string& mode = { })
{
   return Wire::GameOp { kind, value, score, feature, seconds, mode };
}

// Slice 5 (protocol 1.1): an authored table is declared, and its games sent, only to a
// daemon whose HelloAck offers authored_tables at minor 1.
DeclareSnapshot MakeAuthoredDeclare(uint64_t epoch)
{
   DeclareSnapshot d = MakeDeclare("", 0, 0, 0, 0);
   d.declare.tablePath = "Orbit.vpx";
   d.declare.tableKind = Wire::TABLE_AUTHORED;
   d.declare.machineUuid = "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90";
   d.declare.tableFullPath = "/T/Orbit.vpx";
   d.declare.vpxAppPath = "/A/VPinballX";
   d.declare.prefsPath = "/P/VPinballX.ini";
   d.epoch = epoch;
   return d;
}

// Reads Hello and answers a HelloAck with the given minor and capabilities.
bool AckHello(ScorbitTest::TestPeer& peer, uint16_t minor, uint32_t capabilities, Wire::Hello& hello)
{
   Wire::Message msg;
   if (!peer.ReadOfType(Wire::TYPE_HELLO, msg, WAIT_MS) || !Wire::Decode(msg.payload, hello))
      return false;
   Wire::HelloAck ack;
   ack.protoMajor = Wire::PROTO_MAJOR;
   ack.protoMinor = minor;
   ack.daemonVersion = "daemon-test";
   ack.capabilities = capabilities;
   ack.sessionId = 5;
   return peer.Send(Wire::TYPE_HELLO_ACK, Wire::FLAG_RESPONSE, msg.header.seq, Wire::Encode(ack));
}

// The first frame of the given type the plugin sent, from a wire dump.
std::vector<uint8_t> DumpedFrame(const std::string& path, uint16_t type)
{
   std::ifstream in(path, std::ios::binary);
   const std::vector<uint8_t> dump { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
   for (size_t at = 13; at + 13 <= dump.size();)
   {
      const size_t length = dump[at + 9] | (dump[at + 10] << 8) | (dump[at + 11] << 16)
         | (static_cast<size_t>(dump[at + 12]) << 24);
      const size_t frame = at + 13;
      if (frame + length > dump.size())
         break;
      if (dump[at] == '>' && length >= 6 && (dump[frame + 4] | (dump[frame + 5] << 8)) == type)
         return std::vector<uint8_t>(dump.begin() + static_cast<long>(frame), dump.begin() + static_cast<long>(frame + length));
      at = frame + length;
   }
   return { };
}

bool Contains(const std::vector<uint8_t>& bytes, const std::string& text)
{
   return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

void TestAuthoredTableWithAuthoredDaemon()
{
   const std::string dir = MakeTempDir();
   CHECK(!dir.empty());
   if (dir.empty())
      return;
   ScorbitTest::TestPeer peer(dir);
   CHECK(peer.Listening());
   if (!peer.Listening())
      return;
   const std::string dumpPath = dir + "/wire.dump";
   setenv("SCORBIT_WIRE_DUMP", dumpPath.c_str(), 1);

   FakeSource source;
   const DeclareSnapshot authored = MakeAuthoredDeclare(1);
   source.Set(authored, { });
   const Wire::GameBatch start { { Op(Wire::GAME_START, 0), Op(Wire::GAME_BALL, 1) } };
   source.Commit(1, start);

   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));
   CHECK_MSG((hello.capabilities & Wire::CAP_AUTHORED_TABLES) != 0, "the bindings exist, so Hello offers bit 7");
   CHECK(hello.protoMinor == Wire::PROTO_MINOR);

   // --- The authored Declare, 1.1 layout, ahead of any Game ---------------------
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK_MSG(msg.payload == Wire::Encode(authored.declare, 1), "the 1.1 layout, byte for byte");
   Wire::Declare declare;
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK(declare.tableKind == Wire::TABLE_AUTHORED && declare.romId.empty());
   CHECK(declare.machineUuid == authored.declare.machineUuid);
   CHECK(declare.tableFullPath == "/T/Orbit.vpx" && declare.vpxAppPath == "/A/VPinballX"
      && declare.prefsPath == "/P/VPinballX.ini");
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   // --- The Game committed before it, then one refused, which is logged and survived
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   Wire::GameBatch batch;
   CHECK(Wire::Decode(msg.payload, batch) && batch.ops == start.ops);
   CHECK_MSG(batch.batchId == 1, "batch ids count from 1 per plugin load");
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   const Wire::GameBatch score { { Op(Wire::GAME_SCORE, 1, 12340, 0) } };
   source.Commit(1, score);
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, batch) && batch.ops == score.ops && batch.batchId == 2);
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, msg.header.seq,
      Wire::Encode(Wire::ErrorPayload { Wire::ERR_TOO_LARGE, "queue full" })));

   // --- A table change: the old table's last batch goes out before the Declare ending it
   const Wire::GameBatch end { { Op(Wire::GAME_MODE_COMPLETED, 0, 0, 0, 0, "MB:Multiball"), Op(Wire::GAME_END) } };
   source.Commit(1, end);
   source.Set(MakeDeclare("", 0, 0, 0, 0), { });
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK_MSG(Wire::Decode(msg.payload, batch) && batch.ops == end.ops, "the session continues past an error");
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK(declare.tableKind == Wire::TABLE_ROM && declare.machineUuid.empty() && declare.tableFullPath.empty());
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

   // A batch from the table that has gone is dropped, not sent.
   source.Commit(1, score);
   CHECK_MSG(peer.Silent(300), "a stale batch is dropped");

   worker.Stop();
   unsetenv("SCORBIT_WIRE_DUMP");

   // The dump holds the authored Declare with its paths blanked and everything else intact.
   const std::vector<uint8_t> dumped = DumpedFrame(dumpPath, Wire::TYPE_DECLARE);
   CHECK(!dumped.empty());
   CHECK(Contains(dumped, authored.declare.machineUuid) && Contains(dumped, "Orbit.vpx"));
   CHECK(!Contains(dumped, "/T/") && !Contains(dumped, "/A/") && !Contains(dumped, "/P/"));
   Wire::Message redacted;
   CHECK(dumped.size() > Wire::LENGTH_BYTES
      && Wire::DecodeBody(dumped.data() + Wire::LENGTH_BYTES, dumped.size() - Wire::LENGTH_BYTES, redacted));
   CHECK_MSG(Wire::Decode(redacted.payload, declare) && declare.tableFullPath == std::string(12, '*')
      && declare.vpxAppPath == std::string(12, '*') && declare.prefsPath == std::string(16, '*')
      && declare.machineUuid == authored.declare.machineUuid,
      "a redacted Declare still decodes, with only its paths blanked");
   unlink(dumpPath.c_str());
}

// Committed while no daemon listened: a new session gets the latest game from its start,
// never the earlier batches, which have no game_start before them in that session.
void TestGamesCommittedBeforeConnecting()
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
   source.Set(MakeAuthoredDeclare(1), { });
   const Wire::GameBatch stale { { Op(Wire::GAME_SCORE, 1, 500, 0) } };
   const Wire::GameBatch start { { Op(Wire::GAME_START, 0) } };
   const Wire::GameBatch score { { Op(Wire::GAME_SCORE, 1, 12340, 0) } };
   source.Commit(1, stale);
   source.Commit(1, start);
   source.Commit(1, score);

   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));

   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   Wire::GameBatch batch;
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK_MSG(Wire::Decode(msg.payload, batch) && batch.ops == start.ops, "the game's start, not the batch before it");
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, batch) && batch.ops == score.ops);
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK_MSG(peer.Silent(300), "nothing else follows");
   worker.Stop();
}

// The daemon acknowledged the game's start, then the session dropped: the batches committed
// meanwhile go on as is after the reconnect, and nothing acknowledged is sent again.
void TestGameContinuesAcrossReconnect()
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
   source.Set(MakeAuthoredDeclare(1), { });
   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet());
   worker.Start();

   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   const Wire::GameBatch start { { Op(Wire::GAME_START, 0), Op(Wire::GAME_MODE_COMPLETED, 0, 0, 0, 0, "MB:Multiball") } };
   source.Commit(1, start);
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.Silent(200));

   peer.CloseClient();
   const Wire::GameBatch tail { { Op(Wire::GAME_SCORE, 1, 12340, 0) } };
   source.Commit(1, tail);

   CHECK_MSG(peer.Accept(WAIT_MS), "the worker reconnects");
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   Wire::GameBatch batch;
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK_MSG(Wire::Decode(msg.payload, batch) && batch.ops == tail.ops,
      "the tail of a game whose start was acknowledged, not the acknowledged start again");
   CHECK_MSG(batch.batchId == 2, "the id runs on across the reconnect");
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK_MSG(peer.Silent(300), "nothing acknowledged is replayed");
   worker.Stop();
}

// A batch in flight when the session dropped, never acknowledged, is replayed after the
// reconnect with its original batch_id, so the daemon can skip it if it had applied it.
void TestInFlightGameKeepsItsId()
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
   source.Set(MakeAuthoredDeclare(1), { });
   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet());
   worker.Start();

   CHECK(peer.Accept(WAIT_MS));
   Wire::Hello hello;
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   const Wire::GameBatch start { { Op(Wire::GAME_START, 0), Op(Wire::GAME_MODE_COMPLETED, 0, 0, 0, 0, "MB:Multiball") } };
   source.Commit(1, start);
   Wire::GameBatch first;
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK(Wire::Decode(msg.payload, first));
   peer.CloseClient(); // before any acknowledgement

   CHECK_MSG(peer.Accept(WAIT_MS), "the worker reconnects");
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES, hello));
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   Wire::GameBatch again;
   CHECK(peer.ReadOfType(Wire::TYPE_GAME, msg, WAIT_MS));
   CHECK_MSG(Wire::Decode(msg.payload, again) && again == first, "the same batch, batch_id included");
   CHECK(peer.Send(Wire::TYPE_GAME, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK_MSG(peer.Silent(300), "sent once more, not again");
   worker.Stop();
}

void TestAuthoredTableWithoutAuthoredDaemon()
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
   source.Set(MakeAuthoredDeclare(1), { });
   source.Commit(1, Wire::GameBatch { { Op(Wire::GAME_START, 0) } });

   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet());
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   CHECK(AckHello(peer, 1, Wire::CAP_IDENTIFY_FRAMES, hello));
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   Wire::Declare declare;
   CHECK(Wire::Decode(msg.payload, declare));
   CHECK_MSG(declare.tableKind == Wire::TABLE_ROM && declare.machineUuid.empty(), "declared as kind 0");
   CHECK_MSG(declare.tableFullPath.empty() && declare.vpxAppPath.empty() && declare.prefsPath.empty(),
      "no paths to a daemon that does not serve authored tables");
   CHECK(declare.tablePath == "Orbit.vpx");
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK_MSG(peer.Silent(300), "and no Game");
   worker.Stop();
}

// --- Slice 6 read-backs (protocol 1.2) -------------------------------------------

// Every frame of the given direction and type in a wire dump, in order.
std::vector<std::vector<uint8_t>> DumpedFrames(const std::string& path, uint8_t direction, uint16_t type)
{
   std::ifstream in(path, std::ios::binary);
   const std::vector<uint8_t> dump { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
   std::vector<std::vector<uint8_t>> out;
   for (size_t at = 13; at + 13 <= dump.size();)
   {
      const size_t length = dump[at + 9] | (dump[at + 10] << 8) | (dump[at + 11] << 16)
         | (static_cast<size_t>(dump[at + 12]) << 24);
      const size_t frame = at + 13;
      if (frame + length > dump.size())
         break;
      if (dump[at] == direction && length >= 6 && (dump[frame + 4] | (dump[frame + 5] << 8)) == type)
         out.emplace_back(dump.begin() + static_cast<long>(frame), dump.begin() + static_cast<long>(frame + length));
      at = frame + length;
   }
   return out;
}

// The latest players the worker posted, waiting for a table that passes the test.
template <typename Pred>
bool AwaitPlayers(ReadBackChannel& channel, Pred pred, PlayersPost& out)
{
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WAIT_MS);
   while (std::chrono::steady_clock::now() < deadline)
   {
      if (std::unique_ptr<PlayersPost> p = channel.TakePlayers())
      {
         out = *p;
         if (pred(out))
            return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   return false;
}

// Waits for the result of one request, keeping any others that arrive in results.
bool AwaitScores(ReadBackChannel& channel, uint32_t id, TopScoresPost& out, std::deque<TopScoresPost>& results)
{
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WAIT_MS);
   while (std::chrono::steady_clock::now() < deadline)
   {
      channel.TakeResults(results);
      for (auto it = results.begin(); it != results.end(); ++it)
         if (it->requestId == id)
         {
            out = std::move(*it);
            results.erase(it);
            return true;
         }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   return false;
}

bool AwaitLeaderboards(const ReadBackChannel& channel, bool want)
{
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WAIT_MS);
   while (std::chrono::steady_clock::now() < deadline)
   {
      if (channel.Leaderboards() == want)
         return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   return false;
}

// Sends a daemon request and reads its empty acknowledgement.
bool DaemonRequest(ScorbitTest::TestPeer& peer, uint16_t type, uint32_t seq, const std::vector<uint8_t>& payload)
{
   Wire::Message msg;
   return peer.Send(type, 0, seq, payload) && peer.ReadOfType(type, msg, WAIT_MS) && msg.header.seq == seq
      && msg.header.IsResponse() && !msg.header.IsError() && msg.payload.empty();
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void WriteFile(const std::filesystem::path& path, const std::string& bytes)
{
   std::ofstream out(path, std::ios::binary);
   out << bytes;
}

size_t FilesIn(const std::filesystem::path& dir)
{
   std::error_code ec;
   size_t n = 0;
   for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
      n++;
   return n;
}

constexpr uint32_t READ_BACK_CAPS = Wire::CAP_LEADERBOARDS | Wire::CAP_PLAYERS | Wire::CAP_PLAYER_PICTURES;

// A 1.2 daemon serving all three read-backs: negotiation, a leaderboard result and a refusal,
// players and their pictures copied before the acknowledgement, the hidden indicator's Status,
// the dump's redaction, a table switch and the session's end.
void TestReadBacksWithA12Daemon()
{
   const std::string dir = MakeTempDir();
   const std::string files = MakeTempDir();
   CHECK(!dir.empty() && !files.empty());
   if (dir.empty() || files.empty())
      return;
   const std::filesystem::path pictures = std::filesystem::path(files) / "pictures";
   const std::filesystem::path source = std::filesystem::path(files) / "daemon-1-1.jpg";
   const std::filesystem::path source2 = std::filesystem::path(files) / "daemon-1-2.jpg";
   WriteFile(source, "\xff\xd8 first picture");
   WriteFile(source2, "\xff\xd8 second picture");
   {
      ScorbitTest::TestPeer peer(dir);
      CHECK(peer.Listening());
      if (!peer.Listening())
         return;
      const std::string dumpPath = files + "/wire.dump";
      setenv("SCORBIT_WIRE_DUMP", dumpPath.c_str(), 1);

      FakeSource source1;
      source1.Set(MakeAuthoredDeclare(1), { });
      StatusLatch latch;
      ReadBackChannel channel;
      SocketWorkerConfig cfg = MakeConfig(peer);
      cfg.authoredTables = true;
      cfg.pictureDir = pictures.string();
      SocketWorker worker(source1, cfg, Quiet(), nullptr, nullptr, &latch, &channel);
      worker.Start();
      CHECK(peer.Accept(WAIT_MS));

      // --- Negotiation: bits 8 to 10 both ways at minor 2 ------------------------
      Wire::Hello hello;
      CHECK(AckHello(peer, 2, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES | READ_BACK_CAPS, hello));
      CHECK(hello.protoMinor == 2);
      CHECK_MSG((hello.capabilities & READ_BACK_CAPS) == READ_BACK_CAPS, "caps " + std::to_string(hello.capabilities));
      CHECK_MSG(channel.Request(1, { }) == 0, "no request before the Declare");
      Wire::Message msg;
      CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
      CHECK(AwaitLeaderboards(channel, true));

      // --- A leaderboard request, then its result as a separate message ------------
      Wire::TopScoresRequest ask { 0, Wire::SCOPE_VARIANT, Wire::PERIOD_DAYS_30, Wire::VPIN_ONLY, 5, "" };
      const uint32_t id = channel.Request(1, ask);
      CHECK(id != 0);
      CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES_REQUEST, msg, WAIT_MS));
      Wire::TopScoresRequest sent;
      ask.requestId = id;
      CHECK(Wire::Decode(msg.payload, sent) && sent == ask);
      CHECK(peer.Send(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE, msg.header.seq, { }));

      Wire::TopScores scores { id, Wire::TOP_SCORES_OK, 12, { { 1, 123456789, "Ace Player", "ACE", 3, "2026-10-29T18:04:11Z" } } };
      CHECK(DaemonRequest(peer, Wire::TYPE_TOP_SCORES, 500, Wire::Encode(scores)));
      TopScoresPost post;
      std::deque<TopScoresPost> others;
      CHECK(AwaitScores(channel, id, post, others));
      CHECK(post.epoch == 1 && post.result == Wire::TOP_SCORES_OK && post.ageS == 12 && post.entries == scores.entries);
      CHECK_MSG(DaemonRequest(peer, Wire::TYPE_TOP_SCORES, 501, Wire::Encode(scores)), "a result for no pending request is still acknowledged");

      // A refusal surfaces as a result the script can read.
      const uint32_t refused = channel.Request(1, ask);
      CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES_REQUEST, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, msg.header.seq,
         Wire::Encode(Wire::ErrorPayload { Wire::ERR_NOT_DECLARED, "no table" })));
      CHECK(AwaitScores(channel, refused, post, others));
      CHECK(post.result == SCORES_NOT_DECLARED && post.entries.empty());

      // A malformed TopScores is refused and the session goes on.
      CHECK(peer.Send(Wire::TYPE_TOP_SCORES, 0, 502, ScorbitTest::Unhex("01000000" "09")));
      CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES, msg, WAIT_MS));
      CHECK(ErrorCode(msg) == Wire::ERR_MALFORMED);

      // --- Players, the golden frame ---------------------------------------------------
      CHECK(peer.SendRaw(ScorbitTest::Unhex("51000000" "4200" "0000" "06000000" "02"
         "01" "0500" "702d313233" "0300" "414345" "0a00" "41636520506c61796572" "0300" "414345" "0000"
         "02" "0000" "0000" "0000" "0000" "1d00" "68747470733a2f2f73636f726269742e6c696e6b2f632f416243313233")));
      CHECK(peer.ReadOfType(Wire::TYPE_PLAYERS, msg, WAIT_MS));
      CHECK(msg.header.seq == 6 && msg.header.IsResponse() && !msg.header.IsError());
      PlayersPost players;
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return p.slots[0].Claimed(); }, players));
      CHECK(players.epoch == 1 && players.slots[0].playerId == "p-123" && players.slots[0].DisplayName() == "ACE");
      CHECK(players.slots[1].claimDeeplink == "https://scorbit.link/c/AbC123" && !players.slots[1].Claimed());
      CHECK(players.slots[2].playerId.empty() && players.slots[2].claimDeeplink.empty());

      // --- A picture is copied before the acknowledgement ---------------------------------
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 510, Wire::Encode(Wire::PlayerPicture { 1, "p-123", source.string() })));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return !p.slots[0].picture.empty(); }, players));
      const std::filesystem::path copy = players.slots[0].picture;
      CHECK_MSG(copy.parent_path() == pictures, copy.string());
      CHECK(ReadFile(copy) == ReadFile(source));
      std::filesystem::remove(source);
      CHECK_MSG(std::filesystem::exists(copy), "the copy outlives the daemon's file");

      // The same path again copies nothing; a claim that is not the slot's is ignored.
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 511, Wire::Encode(Wire::PlayerPicture { 1, "p-123", source.string() })));
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 512, Wire::Encode(Wire::PlayerPicture { 1, "p-999", source2.string() })));
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 513, Wire::Encode(Wire::PlayerPicture { 2, "", source2.string() })));
      CHECK(FilesIn(pictures) == 1);
      CHECK_MSG(!channel.TakePlayers(), "nothing changed, nothing posted");

      // A new picture supersedes the copy; an empty path removes it.
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 515, Wire::Encode(Wire::PlayerPicture { 1, "p-123", source2.string() })));
      CHECK(AwaitPlayers(channel, [&copy](const PlayersPost& p) { return !p.slots[0].picture.empty() && p.slots[0].picture != copy.string(); }, players));
      CHECK_MSG(!std::filesystem::exists(copy), "the superseded copy is deleted");
      const std::filesystem::path copy2 = players.slots[0].picture;
      CHECK(ReadFile(copy2) == ReadFile(source2));
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 516, Wire::Encode(Wire::PlayerPicture { 1, "p-123", "" })));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return p.slots[0].picture.empty(); }, players));
      CHECK(!std::filesystem::exists(copy2) && FilesIn(pictures) == 0);

      // A file that cannot be copied leaves the slot without a picture, and the session goes on.
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 517, Wire::Encode(Wire::PlayerPicture { 1, "p-123", files + "/missing.jpg" })));
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 518, Wire::Encode(Wire::PlayerPicture { 1, "p-123", source2.string() })));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return !p.slots[0].picture.empty(); }, players));
      const std::filesystem::path copy3 = players.slots[0].picture;

      // A Players with the slot claimed by someone else supersedes its picture.
      Wire::Players next { { { 1, "p-456", "", "Cy", "CY", "" } } };
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYERS, 520, Wire::Encode(next)));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return p.slots[0].playerId == "p-456"; }, players));
      CHECK(players.slots[0].picture.empty() && players.slots[0].DisplayName() == "Cy" && players.slots[1].claimDeeplink.empty());
      CHECK_MSG(!std::filesystem::exists(copy3), "a new claim deletes the old claim's picture");
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYER_PICTURE, 521, Wire::Encode(Wire::PlayerPicture { 1, "p-456", source2.string() })));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return !p.slots[0].picture.empty(); }, players));
      const std::filesystem::path copy4 = players.slots[0].picture;

      // --- Hidden: the auth change is stored, and nothing is shown ------------------------
      CHECK(peer.SendRaw(ScorbitTest::Unhex("0d000000" "2100" "0000" "05000000" "04" "00" "0000" "04")));
      CHECK(peer.ReadOfType(Wire::TYPE_STATUS, msg, WAIT_MS));
      CHECK(msg.header.seq == 5 && msg.header.IsResponse() && !msg.header.IsError());
      StatusRecord record;
      CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return r.haveStatus; }, record));
      CHECK(record.status.sessionState == Wire::SESSION_HIDDEN && record.status.hasAuthStatus
         && record.status.authStatus == Wire::AUTH_PAIRED);
      StatusNotifier notifier(std::chrono::steady_clock::now());
      CHECK_MSG(notifier.Frame(std::make_unique<StatusRecord>(record), true, std::chrono::steady_clock::now()).empty(),
         "hidden posts no notification");
      Wire::Status authOnly = record.status;
      authOnly.authStatus = Wire::AUTH_FAILED;
      CHECK(DaemonRequest(peer, Wire::TYPE_STATUS, 530, Wire::Encode(authOnly)));
      CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return r.status.authStatus == Wire::AUTH_FAILED; }, record));
      CHECK_MSG(notifier.Frame(std::make_unique<StatusRecord>(record), true, std::chrono::steady_clock::now()).empty(),
         "an auth change while hidden still posts nothing");
      CHECK(peer.Send(Wire::TYPE_STATUS, 0, 531, ScorbitTest::Unhex("00" "00" "0000" "06")));
      CHECK(peer.ReadOfType(Wire::TYPE_STATUS, msg, WAIT_MS));
      CHECK_MSG(ErrorCode(msg) == Wire::ERR_MALFORMED, "auth_status 6");

      // --- A table switch drops players, pictures and pending requests ---------------------
      const uint32_t pending = channel.Request(1, ask);
      CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES_REQUEST, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE, msg.header.seq, { }));
      DeclareSnapshot nextTable = MakeAuthoredDeclare(2);
      nextTable.declare.machineUuid = "1b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90";
      source1.Set(nextTable, { });
      CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return p.epoch == 2; }, players));
      CHECK(!players.slots[0].Claimed() && players.slots[0].picture.empty());
      CHECK_MSG(!std::filesystem::exists(copy4), "a table switch deletes the copies");
      CHECK(AwaitScores(channel, pending, post, others));
      CHECK_MSG(post.result == SCORES_LOST && post.epoch == 1, "the old table's request ends, under its own epoch");
      // A request queued under the old epoch after the switch is never sent.
      channel.Request(1, ask);
      CHECK_MSG(peer.Silent(300), "a stale request is dropped");

      // --- Disconnect: players gone, pending requests lost, leaderboards unavailable --------
      CHECK(DaemonRequest(peer, Wire::TYPE_PLAYERS, 540, Wire::Encode(next)));
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return p.slots[0].Claimed(); }, players));
      const uint32_t lost = channel.Request(2, ask);
      CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES_REQUEST, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE, msg.header.seq, { }));
      peer.CloseClient();
      CHECK(AwaitPlayers(channel, [](const PlayersPost& p) { return !p.slots[0].Claimed(); }, players));
      CHECK(AwaitScores(channel, lost, post, others));
      CHECK(post.result == SCORES_LOST && post.epoch == 2);
      CHECK(AwaitLeaderboards(channel, false));
      CHECK(channel.Request(2, ask) == 0);
      worker.Stop();
      unsetenv("SCORBIT_WIRE_DUMP");

      // --- The dump blanks the deeplink and every picture path, and still decodes ------------
      const std::vector<std::vector<uint8_t>> dumpedPlayers = DumpedFrames(dumpPath, '<', Wire::TYPE_PLAYERS);
      CHECK(!dumpedPlayers.empty());
      if (!dumpedPlayers.empty())
      {
         const std::vector<uint8_t>& f = dumpedPlayers.front();
         CHECK(!Contains(f, "scorbit.link") && Contains(f, "Ace Player") && Contains(f, "p-123"));
         Wire::Message redacted;
         Wire::Players back;
         CHECK(Wire::DecodeBody(f.data() + Wire::LENGTH_BYTES, f.size() - Wire::LENGTH_BYTES, redacted)
            && Wire::Decode(redacted.payload, back) && back.slots.size() == 2 && back.slots.size() == 2
            && back.slots[1].claimDeeplink == std::string(29, '*'));
      }
      const std::vector<std::vector<uint8_t>> dumpedPictures = DumpedFrames(dumpPath, '<', Wire::TYPE_PLAYER_PICTURE);
      CHECK(dumpedPictures.size() >= 8);
      for (const std::vector<uint8_t>& f : dumpedPictures)
         CHECK_MSG(!Contains(f, files), "a picture path in the dump");
      unlink(dumpPath.c_str());
   }
   std::error_code ec;
   std::filesystem::remove_all(files, ec);
}

// An older daemon: no 64 is ever sent and the store stays empty; a newer one without the bits,
// or a 1.1 one claiming them, has 65 to 67 refused as unknown.
void TestReadBacksWithOlderDaemons()
{
   for (const uint16_t minor : { uint16_t(1), uint16_t(2) })
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
      source.Set(MakeAuthoredDeclare(1), { });
      StatusLatch latch;
      ReadBackChannel channel;
      SocketWorkerConfig cfg = MakeConfig(peer);
      cfg.authoredTables = true;
      SocketWorker worker(source, cfg, Quiet(), nullptr, nullptr, &latch, &channel);
      worker.Start();
      CHECK(peer.Accept(WAIT_MS));

      Wire::Hello hello;
      // A 1.1 daemon offering the bits still has no 1.2; a 1.2 daemon without them serves none.
      const uint32_t caps = Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES | (minor == 1 ? READ_BACK_CAPS : 0u);
      CHECK(AckHello(peer, minor, caps, hello));
      CHECK((hello.capabilities & READ_BACK_CAPS) == READ_BACK_CAPS);
      Wire::Message msg;
      CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
      CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));

      // A 1.1 Status, trailing byte and all, reads as one without auth_status.
      CHECK(DaemonRequest(peer, Wire::TYPE_STATUS, 40, ScorbitTest::Unhex(minor == 1 ? "01" "00" "0000" "ff" : "01" "00" "0000")));
      StatusRecord record;
      CHECK(AwaitStatus(latch, [](const StatusRecord& r) { return r.haveStatus; }, record));
      CHECK_MSG(!record.status.hasAuthStatus, "minor " + std::to_string(minor));

      const uint16_t types[] = { Wire::TYPE_TOP_SCORES, Wire::TYPE_PLAYERS, Wire::TYPE_PLAYER_PICTURE };
      uint32_t seq = 50;
      for (const uint16_t type : types)
      {
         CHECK(peer.Send(type, 0, seq, ScorbitTest::Unhex("00")));
         CHECK(peer.ReadOfType(type, msg, WAIT_MS));
         CHECK_MSG(ErrorCode(msg) == Wire::ERR_UNSUPPORTED_TYPE && msg.header.seq == seq, "type " + std::to_string(type));
         seq++;
      }
      CHECK(!channel.Leaderboards());
      CHECK_MSG(channel.Request(1, { }) == 0, "no request id without a daemon to answer it");
      CHECK_MSG(peer.Silent(300), "and no TopScoresRequest");
      CHECK(!channel.TakePlayers());
      worker.Stop();
   }
}

// Leaderboards need a table that plays, not the chooser's empty Declare; and a TopScores that
// overtakes its request's acknowledgement is still delivered.
void TestLeaderboardsNeedATable()
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
   DeclareSnapshot chooser = MakeDeclare("", 0, 0, 0, 0);
   chooser.declare.tablePath.clear();
   chooser.epoch = 2;
   source.Set(chooser, { });
   ReadBackChannel channel;
   SocketWorkerConfig cfg = MakeConfig(peer);
   cfg.authoredTables = true;
   SocketWorker worker(source, cfg, Quiet(), nullptr, nullptr, nullptr, &channel);
   worker.Start();
   CHECK(peer.Accept(WAIT_MS));

   Wire::Hello hello;
   CHECK(AckHello(peer, 2, Wire::CAP_IDENTIFY_FRAMES | Wire::CAP_AUTHORED_TABLES | READ_BACK_CAPS, hello));
   Wire::Message msg;
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(peer.Send(Wire::TYPE_PING, 0, 70, { }) && peer.ReadOfType(Wire::TYPE_PONG, msg, WAIT_MS));
   std::this_thread::sleep_for(std::chrono::milliseconds(250));
   CHECK_MSG(!channel.Leaderboards(), "the chooser's Declare names no table");
   CHECK_MSG(channel.Request(2, { }) == 0, "no id at the table chooser");

   DeclareSnapshot table = MakeDeclare("", 0, 0, 0, 0);
   table.declare.tablePath = "Orbit.vpx";
   table.epoch = 3;
   source.Set(table, { });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(AwaitLeaderboards(channel, true));

   const Wire::TopScoresRequest ask { 0, Wire::SCOPE_MACHINE, Wire::PERIOD_ALL_TIME, Wire::VPIN_ANY, 3, "" };
   const uint32_t id = channel.Request(3, ask);
   CHECK(id != 0);
   CHECK(peer.ReadOfType(Wire::TYPE_TOP_SCORES_REQUEST, msg, WAIT_MS));
   const uint32_t requestSeq = msg.header.seq;
   // The result first, while the worker still waits on the request's acknowledgement.
   const Wire::TopScores scores { id, Wire::TOP_SCORES_OK, 4, { { 1, 42, "Ace", "A", 0, "" } } };
   CHECK(DaemonRequest(peer, Wire::TYPE_TOP_SCORES, 71, Wire::Encode(scores)));
   CHECK(peer.Send(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE, requestSeq, { }));
   TopScoresPost post;
   std::deque<TopScoresPost> others;
   CHECK(AwaitScores(channel, id, post, others));
   CHECK_MSG(post.result == Wire::TOP_SCORES_OK && post.entries == scores.entries && post.epoch == 3,
      "a result overtaking the acknowledgement is kept");
   CHECK(post.received != std::chrono::steady_clock::time_point { });

   // Back at the chooser, leaderboards are unavailable again.
   chooser.epoch = 4;
   source.Set(chooser, { });
   CHECK(peer.ReadOfType(Wire::TYPE_DECLARE, msg, WAIT_MS));
   CHECK(peer.Send(Wire::TYPE_DECLARE, Wire::FLAG_RESPONSE, msg.header.seq, { }));
   CHECK(AwaitLeaderboards(channel, false));
   worker.Stop();
}

// Per user, under the platform temp or runtime directory.
void TestPictureDirectoryDefault()
{
   const std::filesystem::path dir = DefaultPictureDirectory();
   CHECK(!dir.empty());
   CHECK(dir.filename() == "pictures" && dir.parent_path().filename() == "scorbit-vpx");
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
   TestAuthoredTableWithAuthoredDaemon();
   TestGamesCommittedBeforeConnecting();
   TestGameContinuesAcrossReconnect();
   TestInFlightGameKeepsItsId();
   TestAuthoredTableWithoutAuthoredDaemon();
   TestReadBacksWithA12Daemon();
   TestReadBacksWithOlderDaemons();
   TestLeaderboardsNeedATable();
   TestPictureDirectoryDefault();
   return ScorbitTest::Summary("socket_worker_test");
}
