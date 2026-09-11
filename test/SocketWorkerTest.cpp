// license:GPLv3+

// Drives a real SocketWorker against the scripted peer in TestPeer.h: the whole
// session from connect to Bye, without Visual Pinball and without a daemon.

#include "SocketWorker.h"

#include "Check.h"
#include "TestPeer.h"
#include "WireVectors.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
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

// Hello and the populated Declare are withheld from this public repo: one carries the
// daemon session token, the other a tablePath naming a home directory and an email
// address. They live beside the corpus and are loaded only when SCORBIT_WIRE_VECTORS
// points at that file. Absent, the checks that need them skip.
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

   // --- The withheld pair, only when the corpus file is pointed at ------------
   const std::string helloHex = PrivateVector("HELLO");
   const std::string declareHex = PrivateVector("DECLARE");
   if (helloHex.empty() || declareHex.empty())
   {
      fprintf(stderr, "skip: Hello and populated Declare need SCORBIT_WIRE_VECTORS "
                      "(they carry a session token and a personal path, so they are "
                      "not in this repo)\n");
      return;
   }

   CHECK(OpenVector("HELLO", helloHex, msg));
   Wire::Hello hello { };
   CHECK(Wire::Decode(msg.payload, hello));
   CHECK_MSG(Wire::Encode(hello) == msg.payload, "Hello re encode is byte identical");
   CHECK(hello.protoMajor == Wire::PROTO_MAJOR && hello.protoMinor == Wire::PROTO_MINOR);
   CHECK(hello.vpxRevision == "5589");

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

int main()
{
   TestSession();
   TestNoFrameHeldSentinel();
   TestOversizedLength();
   TestMissingToken();
   TestErrorWithOnlyTheErrorBit();
   TestCapturedWireBytes();
   TestHandshakeAgainstCapturedAck();
   return ScorbitTest::Summary("socket_worker_test");
}
