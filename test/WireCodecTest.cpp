// license:GPLv3+

// Round trips every slice 1 message and pins the byte layout of the ones most
// easily misread, against scorbitd's
// docs/openspec/specs/vpx-virtual-probe/design.md, subsection "Message layouts
// (protocol 1.0, slice 1)", revision 6edf82c. The daemon's SocketCable is built
// from that same subsection, so the golden vectors here are the thing the two
// sides can be diffed against without running either of them.

#include "WireProtocol.h"

#include "Check.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Scorbit;

namespace
{

const std::string TOKEN =
   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

void TestFraming()
{
   // A Ping request: length 8, type 5, flags 0, seq 7, no payload.
   const std::vector<uint8_t> ping = Wire::EncodeMessage(Wire::TYPE_PING, 0, 7, { });
   CHECK_MSG(ScorbitTest::Hex(ping) == "0800000005000000" "07000000", ScorbitTest::Hex(ping));

   Wire::Message msg;
   CHECK(Wire::DecodeBody(ping.data() + Wire::LENGTH_BYTES, ping.size() - Wire::LENGTH_BYTES, msg));
   CHECK(msg.header.type == Wire::TYPE_PING);
   CHECK(msg.header.flags == 0);
   CHECK(msg.header.seq == 7);
   CHECK(msg.payload.empty());

   // An error response sets both flags and echoes the request's type and seq.
   Wire::ErrorPayload err;
   err.code = Wire::ERR_UNSUPPORTED_TYPE;
   err.reason = "no";
   const std::vector<uint8_t> refused = Wire::EncodeMessage(Wire::TYPE_OVERLAY,
      Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, 9, Wire::Encode(err));
   CHECK_MSG(ScorbitTest::Hex(refused) == "0e000000" "2000" "0300" "09000000" "0100" "0200" "6e6f",
      ScorbitTest::Hex(refused));
   CHECK(Wire::DecodeBody(refused.data() + Wire::LENGTH_BYTES, refused.size() - Wire::LENGTH_BYTES, msg));
   CHECK(msg.header.IsResponse() && msg.header.IsError());
   Wire::ErrorPayload back;
   CHECK(Wire::Decode(msg.payload, back));
   CHECK(back == err);
}

void TestHello()
{
   Wire::Hello hello;
   hello.pluginVersion = "0.1.0";
   hello.vpxRevision = "5589";
   hello.pluginApiCommit = "abc";
   hello.token = TOKEN;
   hello.instanceId = 0x0123456789abcdefULL;
   hello.capabilities = Wire::CAP_IDENTIFY_FRAMES;
   hello.info = "";

   const std::vector<uint8_t> payload = Wire::Encode(hello);

   std::string tokenHex;
   for (int i = 0; i < 4; i++)
      tokenHex += "30313233343536373839616263646566";
   const std::string expected =
      "0100"              // proto_major
      "0000"              // proto_minor
      "0500" "302e312e30" // plugin_version "0.1.0"
      "0400" "35353839"   // vpx_revision "5589"
      "0300" "616263"     // plugin_api_commit "abc"
      "4000" + tokenHex + // token, 64 characters
      "efcdab8967452301"  // instance_id, little endian
      "01000000"          // capabilities, identify_frames
      "0000";             // info, empty
   CHECK_MSG(ScorbitTest::Hex(payload) == expected, ScorbitTest::Hex(payload));

   Wire::Hello back;
   CHECK(Wire::Decode(payload, back));
   CHECK(back == hello);
}

void TestHelloAck()
{
   Wire::HelloAck ack;
   ack.protoMajor = 1;
   ack.protoMinor = 0;
   ack.daemonVersion = "2.7.0";
   ack.capabilities = Wire::CAP_IDENTIFY_FRAMES;
   ack.sessionId = 0xDEADBEEF;

   const std::vector<uint8_t> payload = Wire::Encode(ack);
   CHECK_MSG(ScorbitTest::Hex(payload) == "0100" "0000" "0500" "322e372e30" "01000000" "efbeadde",
      ScorbitTest::Hex(payload));

   Wire::HelloAck back;
   CHECK(Wire::Decode(payload, back));
   CHECK(back == ack);
}

void TestDeclare()
{
   Wire::Declare d;
   d.romId = "ij_l7";
   d.tablePath = "/t.vpx";
   d.vpxVersion = "v";
   d.vpxRevision = "5589";
   d.width = 128;
   d.height = 32;
   d.shades = 4;
   d.sourceGeneration = 3;
   d.identifyFormat = "BITPLANE2";

   const std::vector<uint8_t> payload = Wire::Encode(d);
   const std::string expected =
      "0500" "696a5f6c37"       // rom_id "ij_l7"
      "0600" "2f742e767078"     // table_path "/t.vpx"
      "0100" "76"               // vpx_version "v"
      "0400" "35353839"         // vpx_revision "5589"
      "8000"                    // width 128
      "2000"                    // height 32
      "04"                      // shades
      "03000000"                // source_generation
      "0900" "424954504c414e4532"; // identify_format "BITPLANE2"
   CHECK_MSG(ScorbitTest::Hex(payload) == expected, ScorbitTest::Hex(payload));

   Wire::Declare back;
   CHECK(Wire::Decode(payload, back));
   CHECK(back == d);

   // No conforming source is a legitimate steady state, not an error.
   Wire::Declare none;
   none.vpxRevision = "5589";
   none.sourceGeneration = 12;
   const std::vector<uint8_t> nonePayload = Wire::Encode(none);
   Wire::Declare noneBack;
   CHECK(Wire::Decode(nonePayload, noneBack));
   CHECK(noneBack == none);
   CHECK(noneBack.shades == 0 && noneBack.identifyFormat.empty());
}

void TestFrame()
{
   Wire::FrameRequest req;
   req.sinceFrameId = 0x11223344;
   req.sinceGeneration = 5;
   const std::vector<uint8_t> reqPayload = Wire::Encode(req);
   CHECK_MSG(ScorbitTest::Hex(reqPayload) == "44332211" "05000000", ScorbitTest::Hex(reqPayload));
   Wire::FrameRequest reqBack;
   CHECK(Wire::Decode(reqPayload, reqBack));
   CHECK(reqBack == req);

   // Unchanged: header only, no pixel run at all.
   Wire::FrameReply same;
   same.frameId = 42;
   same.sourceGeneration = 3;
   same.width = 128;
   same.height = 32;
   same.shades = 4;
   same.hasPixels = 0;
   const std::vector<uint8_t> samePayload = Wire::Encode(same);
   CHECK_MSG(ScorbitTest::Hex(samePayload) == "2a000000" "03000000" "8000" "2000" "04" "00",
      ScorbitTest::Hex(samePayload));
   Wire::FrameReply sameBack;
   CHECK(Wire::Decode(samePayload, sameBack));
   CHECK(sameBack == same);

   // Changed: width * height shade indices follow the header, row major.
   Wire::FrameReply full = same;
   full.hasPixels = 1;
   full.pixels.resize(128u * 32u);
   for (size_t i = 0; i < full.pixels.size(); i++)
      full.pixels[i] = static_cast<uint8_t>(i % 4);
   const std::vector<uint8_t> fullPayload = Wire::Encode(full);
   CHECK(fullPayload.size() == 14 + 128u * 32u);
   Wire::FrameReply fullBack;
   CHECK(Wire::Decode(fullPayload, fullBack));
   CHECK(fullBack == full);

   // No source: frame 0, current generation, geometry zeroed.
   Wire::FrameReply absent;
   absent.sourceGeneration = 7;
   const std::vector<uint8_t> absentPayload = Wire::Encode(absent);
   CHECK_MSG(ScorbitTest::Hex(absentPayload) == "00000000" "07000000" "0000" "0000" "00" "00",
      ScorbitTest::Hex(absentPayload));
}

void TestPingPongBye()
{
   Wire::Pong pong;
   pong.renderedFrame = 0x0102030405060708ULL;
   pong.playerRunning = 1;
   pong.writesSupported = 0;
   const std::vector<uint8_t> payload = Wire::Encode(pong);
   CHECK_MSG(ScorbitTest::Hex(payload) == "0807060504030201" "01" "00", ScorbitTest::Hex(payload));
   Wire::Pong pongBack;
   CHECK(Wire::Decode(payload, pongBack));
   CHECK(pongBack == pong);

   Wire::Bye bye;
   bye.reason = "plugin unloading";
   const std::vector<uint8_t> byePayload = Wire::Encode(bye);
   Wire::Bye byeBack;
   CHECK(Wire::Decode(byePayload, byeBack));
   CHECK(byeBack == bye);
}

void TestMalformed()
{
   // A message longer than the bound is never produced.
   const std::vector<uint8_t> huge(Wire::MAX_PAYLOAD_BYTES + 1, 0);
   CHECK(Wire::EncodeMessage(Wire::TYPE_FRAME, 0, 1, huge).empty());
   // One byte under it still encodes, and the whole thing stays inside 1 MiB.
   const std::vector<uint8_t> big(Wire::MAX_PAYLOAD_BYTES, 0);
   const std::vector<uint8_t> encoded = Wire::EncodeMessage(Wire::TYPE_FRAME, 0, 1, big);
   CHECK(!encoded.empty());
   CHECK(encoded.size() == Wire::MAX_MESSAGE_BYTES);

   // A body shorter than a header is not a message.
   Wire::Message msg;
   const uint8_t stub[4] = { 1, 2, 3, 4 };
   CHECK(!Wire::DecodeBody(stub, sizeof(stub), msg));

   // A payload cut short of a field it declares is rejected, not read as zeroes.
   Wire::Declare d;
   d.romId = "ij_l7";
   d.tablePath = "/t.vpx";
   d.vpxVersion = "v";
   d.vpxRevision = "5589";
   d.width = 128;
   d.height = 32;
   d.shades = 4;
   d.sourceGeneration = 3;
   d.identifyFormat = "BITPLANE2";
   std::vector<uint8_t> payload = Wire::Encode(d);
   for (size_t cut = 1; cut < payload.size(); cut++)
   {
      std::vector<uint8_t> truncated(payload.begin(), payload.begin() + static_cast<long>(payload.size() - cut));
      Wire::Declare back;
      CHECK_MSG(!Wire::Decode(truncated, back), "accepted a Declare truncated by " + std::to_string(cut));
   }

   // A string whose length prefix runs past the buffer is rejected.
   std::vector<uint8_t> lying = { 0xFF, 0x00, 'a' };
   Wire::Bye bye;
   CHECK(!Wire::Decode(lying, bye));

   // A pixel run that disagrees with the geometry it follows is refused on
   // encode and on decode, in both directions.
   Wire::FrameReply bad;
   bad.width = 4;
   bad.height = 4;
   bad.hasPixels = 1;
   bad.pixels.resize(15);
   CHECK(Wire::Encode(bad).empty());

   bad.pixels.resize(16);
   std::vector<uint8_t> shortPixels = Wire::Encode(bad);
   CHECK(!shortPixels.empty());
   shortPixels.pop_back();
   Wire::FrameReply badBack;
   CHECK(!Wire::Decode(shortPixels, badBack));

   // A string longer than a u16 length can express is refused rather than cut.
   Wire::Bye giant;
   giant.reason.assign(70000, 'x');
   CHECK(Wire::Encode(giant).empty());
}

// design.md's compatibility rule: appending a field is a minor bump, so a 1.0
// reader must read what it knows and ignore the rest. Refusing trailing bytes
// would make every future minor version look malformed.
void TestForwardCompatibility()
{
   Wire::Declare d;
   d.romId = "ij_l7";
   d.vpxRevision = "5589";
   d.width = 128;
   d.height = 32;
   d.shades = 4;
   d.sourceGeneration = 3;
   d.identifyFormat = "BITPLANE2";
   std::vector<uint8_t> payload = Wire::Encode(d);
   const size_t known = payload.size();

   // A 1.1 daemon appends a field this build has never heard of.
   Wire::Writer extra;
   extra.U32(0xCAFEF00D);
   extra.Str("something new");
   payload.insert(payload.end(), extra.Data().begin(), extra.Data().end());
   CHECK(payload.size() > known);

   Wire::Declare back;
   CHECK_MSG(Wire::Decode(payload, back), "a 1.0 reader must accept an appended field");
   CHECK(back == d);

   // The same for every other payload the plugin reads off the wire.
   auto appended = [](std::vector<uint8_t> bytes)
   {
      bytes.push_back(0x7F);
      return bytes;
   };

   Wire::HelloAck ack;
   ack.protoMajor = 1;
   ack.daemonVersion = "2.7.0";
   ack.sessionId = 9;
   Wire::HelloAck ackBack;
   CHECK(Wire::Decode(appended(Wire::Encode(ack)), ackBack));
   CHECK(ackBack == ack);

   Wire::FrameRequest req { 1, 2 };
   Wire::FrameRequest reqBack;
   CHECK(Wire::Decode(appended(Wire::Encode(req)), reqBack));
   CHECK(reqBack == req);

   Wire::Bye bye { "done" };
   Wire::Bye byeBack;
   CHECK(Wire::Decode(appended(Wire::Encode(bye)), byeBack));
   CHECK(byeBack == bye);

   Wire::ErrorPayload err { Wire::ERR_BUSY, "another instance" };
   Wire::ErrorPayload errBack;
   CHECK(Wire::Decode(appended(Wire::Encode(err)), errBack));
   CHECK(errBack == err);

   // A pixel run is sized by the geometry above it, so trailing bytes come
   // after the run and never eat into it.
   Wire::FrameReply reply;
   reply.frameId = 4;
   reply.sourceGeneration = 1;
   reply.width = 8;
   reply.height = 8;
   reply.shades = 4;
   reply.hasPixels = 1;
   reply.pixels.assign(64, 2);
   Wire::FrameReply replyBack;
   CHECK(Wire::Decode(appended(Wire::Encode(reply)), replyBack));
   CHECK(replyBack == reply);
}

// design.md is explicit that these are two different counters with two
// different owners, and that neither may be compared against the other.
void TestCountersAreDistinct()
{
   Wire::FrameReply reply;
   reply.frameId = 0xFFFFFFFFu;             // DisplayFrame::frameId, u32, may reset
   reply.sourceGeneration = 2;
   Wire::FrameReply replyBack;
   CHECK(Wire::Decode(Wire::Encode(reply), replyBack));
   CHECK(replyBack.frameId == 0xFFFFFFFFu);

   Wire::Pong pong;
   pong.renderedFrame = 0x1FFFFFFFFULL;     // rendered frames, u64, past a u32
   Wire::Pong pongBack;
   CHECK(Wire::Decode(Wire::Encode(pong), pongBack));
   CHECK_MSG(pongBack.renderedFrame == 0x1FFFFFFFFULL, "rendered_frame must survive past 32 bits");
}

}

int main()
{
   TestFraming();
   TestHello();
   TestHelloAck();
   TestDeclare();
   TestFrame();
   TestPingPongBye();
   TestMalformed();
   TestForwardCompatibility();
   TestCountersAreDistinct();
   return ScorbitTest::Summary("wire_codec_test");
}
