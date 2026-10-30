// license:GPLv3+

// Round trips every slice 1 message and pins the byte layout of the ones most
// easily misread, against scorbitd's
// docs/openspec/specs/vpx-virtual-probe/design.md, subsection "Message layouts
// (protocol 1.0, slice 1)", revision 3e8a9d5. The daemon's SocketCable is built
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

   // A peer that sets only the error bit is still sending a response, and the
   // spec requires readers to accept that. Taking one for a request would have
   // this side answer a refusal with a refusal.
   const std::vector<uint8_t> bitOneOnly = Wire::EncodeMessage(Wire::TYPE_HELLO,
      Wire::FLAG_ERROR, 3, Wire::Encode(err));
   CHECK(Wire::DecodeBody(bitOneOnly.data() + Wire::LENGTH_BYTES, bitOneOnly.size() - Wire::LENGTH_BYTES, msg));
   CHECK_MSG(msg.header.IsResponse(), "an error with only bit 1 must classify as a response");
   CHECK(msg.header.IsError());

   // A plain request is neither, and must stay dispatchable.
   const std::vector<uint8_t> request = Wire::EncodeMessage(Wire::TYPE_FRAME, 0, 4,
      Wire::Encode(Wire::FrameRequest { 0, 0 }));
   CHECK(Wire::DecodeBody(request.data() + Wire::LENGTH_BYTES, request.size() - Wire::LENGTH_BYTES, msg));
   CHECK(!msg.header.IsResponse() && !msg.header.IsError());

   // And a plain response is a response but not an error.
   const std::vector<uint8_t> ok = Wire::EncodeMessage(Wire::TYPE_FRAME, Wire::FLAG_RESPONSE, 4, { });
   CHECK(Wire::DecodeBody(ok.data() + Wire::LENGTH_BYTES, ok.size() - Wire::LENGTH_BYTES, msg));
   CHECK(msg.header.IsResponse() && !msg.header.IsError());
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
      "0200"              // proto_minor, 1.2
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

   const std::vector<uint8_t> payload = Wire::Encode(d, 0);
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
   const std::vector<uint8_t> nonePayload = Wire::Encode(none, 0);
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

   // The reserved since_frame_id is a value, not a layout change, so it rides
   // the ordinary encoding. Pinned here because both sides must agree on it.
   CHECK(Wire::SINCE_FRAME_NONE == 0xFFFFFFFFu);
   const std::vector<uint8_t> heldNone = Wire::Encode(Wire::FrameRequest { Wire::SINCE_FRAME_NONE, 7 });
   CHECK_MSG(ScorbitTest::Hex(heldNone) == "ffffffff" "07000000", ScorbitTest::Hex(heldNone));
   Wire::FrameRequest heldNoneBack;
   CHECK(Wire::Decode(heldNone, heldNoneBack));
   CHECK(heldNoneBack.sinceFrameId == Wire::SINCE_FRAME_NONE && heldNoneBack.sinceGeneration == 7);

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
   std::vector<uint8_t> payload = Wire::Encode(d, 0);
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
   std::vector<uint8_t> payload = Wire::Encode(d, Wire::PROTO_MINOR);
   const size_t known = payload.size();

   // A later minor appends a field this build has never heard of.
   Wire::Writer extra;
   extra.U32(0xCAFEF00D);
   extra.Str("something new");
   payload.insert(payload.end(), extra.Data().begin(), extra.Data().end());
   CHECK(payload.size() > known);

   Wire::Declare back;
   CHECK_MSG(Wire::Decode(payload, back), "a 1.1 reader must accept an appended field");
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


// Declare::tablePath must never carry the path VPX hands over. The real one from the
// first live capture named a home directory, an email address and an internal shared
// drive layout, and it reached the daemon's logs and every wire dump. Pinned here
// rather than left to review, because the leak is invisible until someone opens a dump.
void TestTableFileName()
{
   // The shape that caused this, with the identifying parts replaced.
   CHECK(Wire::TableFileName(
      "/Users/someone/Library/CloudStorage/GoogleDrive-someone@example.com/Shared drives/"
      "Engineering/Machine Training/Emulations/archive/TOM/Theatre of Magic (Bally 1995) 2.4.vpx")
      == "Theatre of Magic (Bally 1995) 2.4.vpx");

   CHECK(Wire::TableFileName("C:\\Users\\someone\\Tables\\Theatre of Magic.vpx")
      == "Theatre of Magic.vpx");
   CHECK(Wire::TableFileName("/tables/t.vpx") == "t.vpx");
   CHECK(Wire::TableFileName("t.vpx") == "t.vpx");
   CHECK(Wire::TableFileName("") == "");
   CHECK_MSG(Wire::TableFileName("/tables/").empty(), "a trailing separator leaves nothing");

   // Whatever comes out must not contain a separator, which is the property that
   // actually matters: no directory structure crosses the wire.
   const char* paths[] = {
      "/a/b/c.vpx", "C:\\a\\b\\c.vpx", "/a\\b/c.vpx", "relative/path/c.vpx",
   };
   for (const char* p : paths)
      CHECK_MSG(Wire::TableFileName(p).find_first_of("/\\") == std::string::npos, p);
}

}

// The slice 2 byte strings are copied verbatim from scorbitd main (ffd58ca)
// scorbitd/test/source/test_vpx_wire_layout.cpp, "The slice two reply encodings
// match their golden vectors" and "The slice two requests the daemon sends match
// their golden vectors". Replies are what this side encodes; requests are what the
// daemon sends and this side must decode.
void TestSliceTwoGoldenVectors()
{
   const std::vector<uint8_t> marker { 0xde, 0xad, 0xbe, 0xef };
   const Wire::MemoryRange range { 0x1c93, 4 };
   constexpr uint64_t FRAME = 0x0102030405060708ULL;

   const std::vector<uint8_t> subscribe = Wire::Encode(Wire::Subscription { { range } });
   CHECK_MSG(ScorbitTest::Hex(subscribe) == "0100" "931c0000" "04000000", ScorbitTest::Hex(subscribe));

   const std::vector<uint8_t> poll = Wire::Encode(Wire::PollReply { FRAME, 1, { { 0x1c93, marker } } });
   CHECK_MSG(ScorbitTest::Hex(poll) == "0807060504030201" "01" "0100" "931c0000" "04000000" "deadbeef",
      ScorbitTest::Hex(poll));

   const std::vector<uint8_t> stable = Wire::Encode(Wire::ReadDirectReply { FRAME, 0x1c93, { }, marker });
   CHECK_MSG(ScorbitTest::Hex(stable) == "0807060504030201" "931c0000" "04000000" "0000" "deadbeef",
      ScorbitTest::Hex(stable));

   // One unstable chunk, at offset 0 from the range start.
   const std::vector<uint8_t> unstable = Wire::Encode(Wire::ReadDirectReply { FRAME, 0x1c93, { 0 }, marker });
   CHECK_MSG(ScorbitTest::Hex(unstable) == "0807060504030201" "931c0000" "04000000" "0100" "00000000" "deadbeef",
      ScorbitTest::Hex(unstable));

   // Requests, as the daemon puts them on the wire.
   Wire::ReadDirectRequest readDirect;
   CHECK(Wire::Decode(ScorbitTest::Unhex("931c0000" "04000000"), readDirect));
   CHECK(readDirect == (Wire::ReadDirectRequest { 0x1c93, 4 }));

   Wire::Subscription subscribed;
   CHECK(Wire::Decode(ScorbitTest::Unhex("0200" "931c0000" "04000000" "00010000" "08000000"), subscribed));
   CHECK(subscribed == (Wire::Subscription { { { 0x1c93, 4 }, { 0x0100, 8 } } }));

   Wire::Subscription cleared;
   CHECK(Wire::Decode(ScorbitTest::Unhex("0000"), cleared));
   CHECK(cleared.ranges.empty());
}

// Round trips, plus the truncations a decoder must refuse rather than read as zeroes.
void TestSliceTwoRoundTripsAndTruncation()
{
   const Wire::PollReply poll { 7, 0, { { 0x10, { 1, 2, 3 } }, { 0x2000, { } } } };
   Wire::PollReply pollBack;
   CHECK(Wire::Decode(Wire::Encode(poll), pollBack));
   CHECK(pollBack == poll);

   const Wire::ReadDirectReply read { 9, 0x8000, { 0, 256 }, std::vector<uint8_t>(300, 0x5a) };
   Wire::ReadDirectReply readBack;
   CHECK(Wire::Decode(Wire::Encode(read), readBack));
   CHECK(readBack == read);

   // A count promising more ranges than the payload holds.
   Wire::Subscription sub;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0200" "931c0000" "04000000"), sub));
   // A count so large that trusting it would mean a huge allocation.
   CHECK(!Wire::Decode(ScorbitTest::Unhex("ffff"), sub));
   // A ReadDirect request short of its length field.
   Wire::ReadDirectRequest req;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("931c0000"), req));
   // A block whose length runs past the end of the payload.
   Wire::PollReply shortPoll;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0807060504030201" "01" "0100" "931c0000" "08000000" "deadbeef"), shortPoll));
   // An unstable count the payload cannot hold.
   Wire::ReadDirectReply shortRead;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0807060504030201" "931c0000" "00000000" "ffff"), shortRead));
}

// scorbitd test_vpx_wire_layout.cpp, verbatim: a WPC credit write, the daemon putting the
// unlock in the batch, and the NvRam it sends just before.
void TestWriteGoldenVectors()
{
   Wire::WriteRequest write;
   CHECK(Wire::Decode(ScorbitTest::Unhex("0200" "fd3f0000" "0100" "b4" "931c0000" "0200" "0801"), write));
   CHECK(write == (Wire::WriteRequest { { { 0x3ffd, { 0xb4 } }, { 0x1c93, { 0x08, 0x01 } } } }));

   const std::vector<uint8_t> reply = Wire::Encode(Wire::WriteReply { 0x0102030405060708ULL, 2 });
   CHECK_MSG(ScorbitTest::Hex(reply) == "0807060504030201" "0200", ScorbitTest::Hex(reply));

   Wire::NvRamParameters nvram;
   CHECK(Wire::Decode(ScorbitTest::Unhex("00180000" "00080000" "fd3f0000" "b4" "b4"), nvram));
   CHECK(nvram == (Wire::NvRamParameters { 0x1800, 0x800, 0x3ffd, 0xb4, 0xb4 }));
}

void TestWriteRoundTripsAndTruncation()
{
   const Wire::WriteRequest write { { { 0x10, { 1, 2, 3 } }, { 0x2000, { } } } };
   Wire::WriteRequest writeBack;
   CHECK(Wire::Decode(Wire::Encode(write), writeBack));
   CHECK(writeBack == write);

   const Wire::NvRamParameters nvram { 1, 2, 3, 4, 5 };
   Wire::NvRamParameters nvramBack;
   CHECK(Wire::Decode(Wire::Encode(nvram), nvramBack));
   CHECK(nvramBack == nvram);

   // A count promising more ops than the payload holds, and one too large to trust.
   Wire::WriteRequest req;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0200" "fd3f0000" "0100" "b4"), req));
   CHECK(!Wire::Decode(ScorbitTest::Unhex("ffff"), req));
   // An op whose length runs past the end of the payload.
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0100" "931c0000" "0400" "0801"), req));
   // NvRam short of its lock value.
   Wire::NvRamParameters shortNvram;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("00180000" "00080000" "fd3f0000" "b4"), shortNvram));
}

std::string Repeat(const char* hexByte, size_t count)
{
   std::string out;
   for (size_t i = 0; i < count; i++)
      out += hexByte;
   return out;
}

// Whole wire frames from scorbitd design.md, "Message layouts (protocol 1.0, slice 4)",
// golden vectors, verbatim. Requests are what the daemon sends and this side decodes;
// acknowledgements and the refusal are what this side encodes.
void TestSliceFourGoldenVectors()
{
   const std::string upload = "6f000000" "2000" "0000" "07000000" "01" "0a" "0a" + Repeat("80", 10) + Repeat("0f", 90);
   const std::string showFor = "0c000000" "2000" "0000" "08000000" "02" "01" "05dc";
   const std::string showUntilHidden = "0c000000" "2000" "0000" "09000000" "02" "01" "0000";
   const std::string hide = "0c000000" "2000" "0000" "0a000000" "02" "00" "0000";
   const std::string status = "13000000" "2100" "0000" "0b000000" "02" "00" "0700" "6e6f2064617461";

   // Each request is exactly what this side would frame, and decodes to what it says.
   std::vector<uint8_t> pixels(10, 0x80);
   pixels.resize(100, 0x0f);
   std::vector<uint8_t> uploadPayload { 0x0a, 0x0a };
   uploadPayload.insert(uploadPayload.end(), pixels.begin(), pixels.end());
   const Wire::OverlayRequest uploadReq { Wire::OVERLAY_OP_UPLOAD, uploadPayload };
   CHECK_MSG(ScorbitTest::Hex(Wire::EncodeMessage(Wire::TYPE_OVERLAY, 0, 7, Wire::Encode(uploadReq))) == upload, "upload");

   const struct
   {
      const std::string* frame;
      uint32_t seq;
      Wire::OverlayControl control;
   } controls[] = {
      { &showFor, 8, { Wire::OVERLAY_CONTROL_ENABLE, 1500 } },
      { &showUntilHidden, 9, { Wire::OVERLAY_CONTROL_ENABLE, 0 } },
      { &hide, 10, { 0, 0 } },
   };
   for (const auto& c : controls)
   {
      const std::vector<uint8_t> bytes = ScorbitTest::Unhex(*c.frame);
      Wire::Message msg;
      CHECK(Wire::DecodeBody(bytes.data() + Wire::LENGTH_BYTES, bytes.size() - Wire::LENGTH_BYTES, msg));
      CHECK(msg.header.type == Wire::TYPE_OVERLAY && msg.header.seq == c.seq && !msg.header.IsResponse());
      Wire::OverlayRequest req;
      CHECK(Wire::Decode(msg.payload, req));
      CHECK(req.op == Wire::OVERLAY_OP_CONTROL);
      Wire::OverlayControl control;
      CHECK(Wire::Decode(req.payload, control));
      CHECK_MSG(control == c.control, "seq " + std::to_string(c.seq) + ": duration " + std::to_string(control.durationMs));
      const Wire::OverlayRequest back { Wire::OVERLAY_OP_CONTROL, Wire::Encode(c.control) };
      CHECK_MSG(ScorbitTest::Hex(Wire::EncodeMessage(Wire::TYPE_OVERLAY, 0, c.seq, Wire::Encode(back))) == *c.frame,
         "seq " + std::to_string(c.seq));
   }

   const std::vector<uint8_t> ack = Wire::EncodeMessage(Wire::TYPE_OVERLAY, Wire::FLAG_RESPONSE, 8, { });
   CHECK_MSG(ScorbitTest::Hex(ack) == "08000000" "2000" "0100" "08000000", ScorbitTest::Hex(ack));

   const std::vector<uint8_t> refused = Wire::EncodeMessage(Wire::TYPE_OVERLAY, Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, 7,
      Wire::Encode(Wire::ErrorPayload { Wire::ERR_UNSUPPORTED_OPERATION, "no DMD source" }));
   CHECK_MSG(ScorbitTest::Hex(refused) == "19000000" "2000" "0300" "07000000" "0600" "0d00" "6e6f20444d4420736f75726365",
      ScorbitTest::Hex(refused));

   {
      const std::vector<uint8_t> bytes = ScorbitTest::Unhex(status);
      Wire::Message msg;
      CHECK(Wire::DecodeBody(bytes.data() + Wire::LENGTH_BYTES, bytes.size() - Wire::LENGTH_BYTES, msg));
      CHECK(msg.header.type == Wire::TYPE_STATUS && msg.header.seq == 11);
      Wire::Status decoded;
      CHECK(Wire::Decode(msg.payload, decoded));
      CHECK(decoded == (Wire::Status { Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "no data" }));
      CHECK(ScorbitTest::Hex(Wire::EncodeMessage(Wire::TYPE_STATUS, 0, 11, Wire::Encode(decoded))) == status);
   }
   const std::vector<uint8_t> statusAck = Wire::EncodeMessage(Wire::TYPE_STATUS, Wire::FLAG_RESPONSE, 11, { });
   CHECK_MSG(ScorbitTest::Hex(statusAck) == "08000000" "2100" "0100" "0b000000", ScorbitTest::Hex(statusAck));
}

void TestSliceFourTruncation()
{
   Wire::OverlayRequest req;
   CHECK_MSG(!Wire::Decode({ }, req), "an Overlay with no op");
   CHECK(Wire::Decode(ScorbitTest::Unhex("01"), req) && req.op == 1 && req.payload.empty());

   Wire::OverlayControl control;
   CHECK_MSG(!Wire::Decode(ScorbitTest::Unhex("0105"), control), "control short of its duration's low byte");
   // Trailing bytes past the control's fields are ignored.
   CHECK(Wire::Decode(ScorbitTest::Unhex("030100ff"), control));
   CHECK(control.control == 3 && control.durationMs == 0x0100);

   Wire::Status status;
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0200"), status));
   CHECK(!Wire::Decode(ScorbitTest::Unhex("0200" "0500" "6e6f"), status));
   // A 1.1 reader ignores trailing bytes; from 1.2 they are auth_status, and 0xff is unknown.
   CHECK(Wire::Decode(ScorbitTest::Unhex("0103" "0000" "ff"), status, 1));
   CHECK(status == (Wire::Status { Wire::SESSION_TRACKING, Wire::SYSTEM_DEGRADED, "" }));
   CHECK_MSG(!Wire::Decode(ScorbitTest::Unhex("0103" "0000" "ff"), status), "an unknown auth_status at 1.2");
   CHECK_MSG(!Wire::Decode(ScorbitTest::Unhex("0103" "0000" "06"), status), "auth_status 6");

   // support_text is bounded at 256 bytes: 256 decodes, 257 does not.
   const Wire::Status atLimit { 0, 0, std::string(Wire::MAX_SUPPORT_TEXT_BYTES, 'x') };
   CHECK(Wire::Decode(Wire::Encode(atLimit), status) && status == atLimit);
   const Wire::Status over { 0, 0, std::string(Wire::MAX_SUPPORT_TEXT_BYTES + 1, 'x') };
   CHECK_MSG(!Wire::Decode(Wire::Encode(over), status), "support_text over 256 bytes is malformed");
}

// A GameOp with the fields its kind does not use left at zero.
Wire::GameOp Op(uint8_t kind, uint8_t value = 0, int64_t score = 0, int32_t feature = 0, uint32_t seconds = 0,
   const std::string& mode = { })
{
   return Wire::GameOp { kind, value, score, feature, seconds, mode };
}

// Whole wire frames from the slice 5 contract (protocol 1.1, SB-5984), §8, verbatim.
void TestSliceFiveGoldenVectors()
{
   const auto frame = [](uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload)
   {
      return ScorbitTest::Hex(Wire::EncodeMessage(Wire::TYPE_GAME, flags, seq, payload));
   };
   const auto decodeGame = [](const std::string& hex, uint32_t seq, Wire::GameBatch& out)
   {
      const std::vector<uint8_t> bytes = ScorbitTest::Unhex(hex);
      Wire::Message msg;
      return Wire::DecodeBody(bytes.data() + Wire::LENGTH_BYTES, bytes.size() - Wire::LENGTH_BYTES, msg)
         && msg.header.type == Wire::TYPE_GAME && msg.header.seq == seq && !msg.header.IsResponse()
         && Wire::Decode(msg.payload, out);
   };

   const struct
   {
      const char* name;
      std::string hex;
      uint32_t seq;
      Wire::GameBatch batch;
   } games[] = {
      { "batch 1, start by button", "10000000" "3000" "0000" "09000000" "01000000" "0100" "01" "00", 9,
         { { Op(Wire::GAME_START, 0) }, 1 } },
      { "batch 2, add expiring for 3 s",
         "21000000" "3000" "0000" "0a000000" "02000000" "0100" "07" "03000000" "0c00" "4d423a4d756c746962616c6c", 10,
         { { Op(Wire::GAME_ADD_MODE_EXPIRING, 0, 0, 0, 3, "MB:Multiball") }, 2 } },
      { "batch 3, ball 1, player 1 scores 12,340",
         "1e000000" "3000" "0000" "0b000000" "03000000" "0200" "03" "01" "05" "01" "3430000000000000" "00000000", 11,
         { { Op(Wire::GAME_BALL, 1), Op(Wire::GAME_SCORE, 1, 12340, 0) }, 3 } },
      { "batch 4, mode completed, then game end",
         "1e000000" "3000" "0000" "0c000000" "04000000" "0200" "08" "0c00" "4d423a4d756c746962616c6c" "02", 12,
         { { Op(Wire::GAME_MODE_COMPLETED, 0, 0, 0, 0, "MB:Multiball"), Op(Wire::GAME_END) }, 4 } },
   };
   for (const auto& g : games)
   {
      const std::string encoded = frame(0, g.seq, Wire::Encode(g.batch));
      CHECK_MSG(encoded == g.hex, std::string(g.name) + ": " + encoded);
      Wire::GameBatch back;
      CHECK_MSG(decodeGame(g.hex, g.seq, back) && back == g.batch, g.name);
   }

   CHECK(frame(Wire::FLAG_RESPONSE, 9, { }) == "08000000" "3000" "0100" "09000000");

   const std::string notAuthored = "0c000000" "3000" "0300" "0a000000" "0d00" "0000";
   CHECK(frame(Wire::FLAG_RESPONSE | Wire::FLAG_ERROR, 10,
      Wire::Encode(Wire::ErrorPayload { Wire::ERR_NOT_AUTHORED, "" })) == notAuthored);
   CHECK(std::string(Wire::ErrorName(Wire::ERR_NOT_AUTHORED)) == "not_authored");
   CHECK(std::string(Wire::TypeName(Wire::TYPE_GAME)) == "Game");

   const std::string uuid = "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90";
   const std::string declareHex = "86000000" "0300" "0000" "03000000"
      "0000" "0900" "4f726269742e767078" "0600" "31302e382e31" "0700" "61316232633364"
      "0000" "0000" "00" "00000000" "0000"
      "01"
      "2400" "30623663326636652d376431612d346333652d396632302d356138653164346237633930"
      "0c00" "2f542f4f726269742e767078"
      "0c00" "2f412f5650696e62616c6c58"
      "1000" "2f502f5650696e62616c6c582e696e69";
   Wire::Declare authored;
   authored.tablePath = "Orbit.vpx";
   authored.vpxVersion = "10.8.1";
   authored.vpxRevision = "a1b2c3d";
   authored.tableKind = Wire::TABLE_AUTHORED;
   authored.machineUuid = uuid;
   authored.tableFullPath = "/T/Orbit.vpx";
   authored.vpxAppPath = "/A/VPinballX";
   authored.prefsPath = "/P/VPinballX.ini";
   std::vector<Wire::SensitiveSpan> spans;
   const std::vector<uint8_t> payload = Wire::Encode(authored, 1, &spans);
   const std::string encoded = ScorbitTest::Hex(Wire::EncodeMessage(Wire::TYPE_DECLARE, 0, 3, payload));
   CHECK_MSG(encoded == declareHex, encoded);
   const std::vector<uint8_t> bytes = ScorbitTest::Unhex(declareHex);
   Wire::Message msg;
   CHECK(Wire::DecodeBody(bytes.data() + Wire::LENGTH_BYTES, bytes.size() - Wire::LENGTH_BYTES, msg));
   Wire::Declare back;
   CHECK(Wire::Decode(msg.payload, back) && back == authored);

   // The spans are each path's UTF-8 bytes, without its length prefix.
   CHECK(spans.size() == 3);
   const std::string* paths[] = { &authored.tableFullPath, &authored.vpxAppPath, &authored.prefsPath };
   for (size_t i = 0; i < spans.size() && i < 3; i++)
   {
      const Wire::SensitiveSpan& span = spans[i];
      CHECK(span.offset + span.length <= payload.size());
      CHECK_MSG(std::string(payload.begin() + static_cast<long>(span.offset),
         payload.begin() + static_cast<long>(span.offset + span.length)) == *paths[i], "span " + std::to_string(i));
   }

   // The same table to a 1.0 peer stops after identify_format, and carries no span.
   const std::vector<uint8_t> old = Wire::Encode(authored, 0, &spans);
   CHECK(old.size() == payload.size() - (1 + 2 + 36 + 2 + 12 + 2 + 12 + 2 + 16));
   CHECK(spans.empty());
   Wire::Declare oldBack;
   CHECK(Wire::Decode(old, oldBack));
   CHECK(oldBack.tableKind == Wire::TABLE_ROM && oldBack.machineUuid.empty() && oldBack.prefsPath.empty());

   // Cut anywhere inside the appended fields, a 1.1 Declare is refused.
   for (size_t keep = old.size() + 1; keep < payload.size(); keep++)
   {
      std::vector<uint8_t> truncated(payload.begin(), payload.begin() + static_cast<long>(keep));
      CHECK_MSG(!Wire::Decode(truncated, back), "accepted a 1.1 Declare cut to " + std::to_string(keep));
   }
}

void TestGameRejections()
{
   const auto rejects = [](const std::string& hex) { Wire::GameBatch out; return !Wire::Decode(ScorbitTest::Unhex(hex), out); };
   // Every payload leads with its u32 batch_id.
   const std::string id = "07000000";
   CHECK_MSG(rejects("070000"), "shorter than a batch_id");
   CHECK_MSG(rejects(id + "0000"), "count 0");
   CHECK_MSG(rejects(id + "0101" + Repeat("0a", 257)), "count 257");
   CHECK_MSG(!rejects(id + "0001" + Repeat("0a", 256)), "count 256");
   CHECK_MSG(rejects(id + "0100" "0b"), "unknown kind 11");
   CHECK_MSG(rejects(id + "0100" "00"), "unknown kind 0");
   CHECK_MSG(rejects(id + "0200" "0a" "0100"), "game_start not first");
   CHECK_MSG(rejects(id + "0200" "02" "0a"), "game_end not last");
   CHECK_MSG(!rejects(id + "0200" "0100" "02"), "start then end");
   CHECK_MSG(rejects(id + "0100" "05" "01" "3430"), "a score cut short");
   CHECK_MSG(rejects(id + "0200" "0a"), "fewer ops than the count");

   // Encode holds to the same rules rather than sending what the daemon refuses.
   CHECK(Wire::Encode(Wire::GameBatch { }).empty());
   CHECK(Wire::Encode(Wire::GameBatch { { Op(Wire::GAME_CLEAR_MODES), Op(Wire::GAME_START) } }).empty());
   CHECK(Wire::Encode(Wire::GameBatch { { Op(Wire::GAME_END), Op(Wire::GAME_CLEAR_MODES) } }).empty());
   CHECK(Wire::Encode(Wire::GameBatch { { Op(11) } }).empty());
   CHECK(Wire::Encode(Wire::GameBatch { std::vector<Wire::GameOp>(Wire::MAX_GAME_OPS + 1, Op(Wire::GAME_CLEAR_MODES)) }).empty());

   // A negative score and feature survive the trip as two's complement.
   const Wire::GameBatch negative { { Op(Wire::GAME_SCORE, 2, -5, -1) } };
   Wire::GameBatch back;
   CHECK(Wire::Decode(Wire::Encode(negative), back) && back == negative);
}

// A whole frame, split into its message; false unless it is the type and seq given, as a request.
bool DecodeFrame(const std::string& hex, uint16_t type, uint32_t seq, Wire::Message& msg)
{
   const std::vector<uint8_t> bytes = ScorbitTest::Unhex(hex);
   return bytes.size() > Wire::LENGTH_BYTES
      && Wire::DecodeBody(bytes.data() + Wire::LENGTH_BYTES, bytes.size() - Wire::LENGTH_BYTES, msg)
      && msg.header.type == type && msg.header.seq == seq && !msg.header.IsResponse();
}

std::string Frame(uint16_t type, uint16_t flags, uint32_t seq, const std::vector<uint8_t>& payload)
{
   return ScorbitTest::Hex(Wire::EncodeMessage(type, flags, seq, payload));
}

// Whole wire frames from the slice 6 contract (protocol 1.2), §9, verbatim.
const std::string TOP_SCORES_REQUEST_HEX = "12000000" "4000" "0000" "14000000" "01000000" "00" "00" "00" "0a" "0000";
const std::string TOP_SCORES_HEX = "6d000000" "4100" "0000" "05000000" "01000000" "00" "00000000" "02"
   "0100" "15cd5b0700000000" "0a00" "41636520506c61796572" "0300" "414345" "03"
   "1400" "323032362d31302d32395431383a30343a31315a"
   "0200" "780ae30500000000" "0200" "426f" "0200" "424f" "00"
   "1400" "323032362d31302d33305430393a31353a30305a";
const std::string PLAYERS_HEX = "51000000" "4200" "0000" "06000000" "02"
   "01" "0500" "702d313233" "0300" "414345" "0a00" "41636520506c61796572" "0300" "414345" "0000"
   "02" "0000" "0000" "0000" "0000" "1d00" "68747470733a2f2f73636f726269742e6c696e6b2f632f416243313233";
const std::string PLAYERS_EMPTY_HEX = "09000000" "4200" "0000" "07000000" "00";
const std::string PICTURE_HEX = "32000000" "4300" "0000" "08000000" "01" "0500" "702d313233"
   "2000" "2f542f7670782d70696374757265732f30303030303032612d312d312e6a7067";
const std::string PICTURE_REMOVED_HEX = "12000000" "4300" "0000" "09000000" "01" "0500" "702d313233" "0000";
const std::string STATUS_UNPAIRED_HEX = "0d000000" "2100" "0000" "04000000" "00" "01" "0000" "03";
const std::string STATUS_HIDDEN_HEX = "0d000000" "2100" "0000" "05000000" "04" "00" "0000" "04";

void TestSliceSixGoldenVectors()
{
   CHECK(Wire::PROTO_MINOR == 2);
   Wire::Message msg;

   // --- TopScoresRequest and its empty response ---------------------------------
   const Wire::TopScoresRequest request { 1, Wire::SCOPE_MACHINE, Wire::PERIOD_ALL_TIME, Wire::VPIN_ANY, 10, "" };
   CHECK_MSG(Frame(Wire::TYPE_TOP_SCORES_REQUEST, 0, 20, Wire::Encode(request)) == TOP_SCORES_REQUEST_HEX,
      Frame(Wire::TYPE_TOP_SCORES_REQUEST, 0, 20, Wire::Encode(request)));
   Wire::TopScoresRequest requestBack;
   CHECK(DecodeFrame(TOP_SCORES_REQUEST_HEX, Wire::TYPE_TOP_SCORES_REQUEST, 20, msg) && Wire::Decode(msg.payload, requestBack)
      && requestBack == request);
   CHECK(Frame(Wire::TYPE_TOP_SCORES_REQUEST, Wire::FLAG_RESPONSE, 20, { }) == "08000000" "4000" "0100" "14000000");

   // --- TopScores ----------------------------------------------------------------
   Wire::TopScores scores;
   CHECK(DecodeFrame(TOP_SCORES_HEX, Wire::TYPE_TOP_SCORES, 5, msg) && Wire::Decode(msg.payload, scores));
   CHECK(scores.requestId == 1 && scores.result == Wire::TOP_SCORES_OK && scores.ageS == 0 && scores.entries.size() == 2);
   if (scores.entries.size() == 2)
   {
      const Wire::TopScoreEntry& a = scores.entries[0];
      CHECK(a.rank == 1 && a.highScore == 123456789 && a.displayName == "Ace Player" && a.initials == "ACE");
      CHECK(a.flags == (Wire::SCORE_IS_VPIN | Wire::SCORE_VERIFIED) && a.created == "2026-10-29T18:04:11Z");
      const Wire::TopScoreEntry& b = scores.entries[1];
      CHECK(b.rank == 2 && b.highScore == 98765432 && b.displayName == "Bo" && b.initials == "BO");
      CHECK(b.flags == 0 && b.created == "2026-10-30T09:15:00Z");
   }
   CHECK_MSG(Frame(Wire::TYPE_TOP_SCORES, 0, 5, Wire::Encode(scores)) == TOP_SCORES_HEX, "TopScores re-encodes byte for byte");

   // --- Players --------------------------------------------------------------------
   Wire::Players players;
   std::vector<Wire::SensitiveSpan> spans;
   CHECK(DecodeFrame(PLAYERS_HEX, Wire::TYPE_PLAYERS, 6, msg) && Wire::Decode(msg.payload, players, &spans));
   CHECK(players.slots.size() == 2);
   if (players.slots.size() == 2)
   {
      const Wire::PlayerSlot& one = players.slots[0];
      CHECK(one.player == 1 && one.playerId == "p-123" && one.preferredName == "ACE" && one.name == "Ace Player"
         && one.initials == "ACE" && one.claimDeeplink.empty());
      const Wire::PlayerSlot& two = players.slots[1];
      CHECK(two.player == 2 && two.playerId.empty() && two.preferredName.empty() && two.name.empty() && two.initials.empty()
         && two.claimDeeplink == "https://scorbit.link/c/AbC123");
   }
   const std::string deeplink = "https://scorbit.link/c/AbC123";
   CHECK_MSG(spans.size() == 1 && spans[0].length == deeplink.size() && spans[0].offset + spans[0].length == msg.payload.size(),
      "the one deeplink, its bytes only");
   std::vector<Wire::SensitiveSpan> encodedSpans;
   CHECK_MSG(Frame(Wire::TYPE_PLAYERS, 0, 6, Wire::Encode(players, &encodedSpans)) == PLAYERS_HEX, "Players re-encodes");
   CHECK(encodedSpans.size() == 1 && spans.size() == 1 && encodedSpans[0].offset == spans[0].offset);

   Wire::Players none;
   CHECK(DecodeFrame(PLAYERS_EMPTY_HEX, Wire::TYPE_PLAYERS, 7, msg) && Wire::Decode(msg.payload, none) && none.slots.empty());
   CHECK(Frame(Wire::TYPE_PLAYERS, 0, 7, Wire::Encode(none)) == PLAYERS_EMPTY_HEX);

   // --- PlayerPicture ----------------------------------------------------------------
   Wire::PlayerPicture picture;
   CHECK(DecodeFrame(PICTURE_HEX, Wire::TYPE_PLAYER_PICTURE, 8, msg) && Wire::Decode(msg.payload, picture, &spans));
   CHECK(picture.player == 1 && picture.playerId == "p-123" && picture.path == "/T/vpx-pictures/0000002a-1-1.jpg");
   CHECK(spans.size() == 1 && spans.size() == 1 && spans[0].offset == 1 + 2 + 5 + 2 && spans[0].length == picture.path.size());
   CHECK_MSG(Frame(Wire::TYPE_PLAYER_PICTURE, 0, 8, Wire::Encode(picture)) == PICTURE_HEX, "PlayerPicture re-encodes");
   Wire::PlayerPicture removed;
   CHECK(DecodeFrame(PICTURE_REMOVED_HEX, Wire::TYPE_PLAYER_PICTURE, 9, msg) && Wire::Decode(msg.payload, removed, &spans));
   CHECK(removed.player == 1 && removed.playerId == "p-123" && removed.path.empty() && spans.empty());
   CHECK(Frame(Wire::TYPE_PLAYER_PICTURE, 0, 9, Wire::Encode(removed)) == PICTURE_REMOVED_HEX);

   // --- Status with the auth_status tail -----------------------------------------------
   Wire::Status status;
   CHECK(DecodeFrame(STATUS_UNPAIRED_HEX, Wire::TYPE_STATUS, 4, msg) && Wire::Decode(msg.payload, status, 2));
   CHECK(status.sessionState == Wire::SESSION_NONE && status.systemState == Wire::SYSTEM_UNPAIRED && status.supportText.empty());
   CHECK(status.hasAuthStatus && status.authStatus == Wire::AUTH_UNPAIRED);
   CHECK(Frame(Wire::TYPE_STATUS, 0, 4, Wire::Encode(status)) == STATUS_UNPAIRED_HEX);
   CHECK(DecodeFrame(STATUS_HIDDEN_HEX, Wire::TYPE_STATUS, 5, msg) && Wire::Decode(msg.payload, status, 2));
   CHECK(status.sessionState == Wire::SESSION_HIDDEN && status.systemState == Wire::SYSTEM_OK);
   CHECK(status.hasAuthStatus && status.authStatus == Wire::AUTH_PAIRED);
   CHECK(Frame(Wire::TYPE_STATUS, 0, 5, Wire::Encode(status)) == STATUS_HIDDEN_HEX);
   // A 1.1 session reads the same frame without the tail; a 1.2 one accepts a Status without it.
   CHECK(Wire::Decode(msg.payload, status, 1) && !status.hasAuthStatus && status.sessionState == Wire::SESSION_HIDDEN);
   CHECK(Wire::Decode(ScorbitTest::Unhex("0400" "0000"), status, 2) && !status.hasAuthStatus);
   for (uint8_t v = 0; v <= Wire::AUTH_FAILED; v++)
   {
      const Wire::Status s { 1, 0, "", true, v };
      Wire::Status back;
      CHECK_MSG(Wire::Decode(Wire::Encode(s), back, 2) && back == s, "auth_status " + std::to_string(v));
   }

   CHECK(std::string(Wire::TypeName(Wire::TYPE_TOP_SCORES_REQUEST)) == "TopScoresRequest");
   CHECK(std::string(Wire::TypeName(Wire::TYPE_TOP_SCORES)) == "TopScores");
   CHECK(std::string(Wire::TypeName(Wire::TYPE_PLAYERS)) == "Players");
   CHECK(std::string(Wire::TypeName(Wire::TYPE_PLAYER_PICTURE)) == "PlayerPicture");
   CHECK(Wire::CAP_LEADERBOARDS == 0x100 && Wire::CAP_PLAYERS == 0x200 && Wire::CAP_PLAYER_PICTURES == 0x400);
   CHECK(Wire::TYPE_GAME_START_REQUESTED == 49 && Wire::TYPE_CREDITS_ADD_REQUESTED == 68 && Wire::TYPE_PRICING == 72);
}

void TestSliceSixRejections()
{
   const auto request = [](const std::string& hex) { Wire::TopScoresRequest out; return Wire::Decode(ScorbitTest::Unhex(hex), out); };
   const std::string id = "01000000";
   CHECK(request(id + "02" "05" "02" "14" "0000"));
   CHECK_MSG(!request(id + "03" "00" "00" "0a" "0000"), "scope 3");
   CHECK_MSG(!request(id + "00" "06" "00" "0a" "0000"), "period 6");
   CHECK_MSG(!request(id + "00" "00" "03" "0a" "0000"), "vpin_filter 3");
   CHECK_MSG(!request(id + "00" "00" "00" "00" "0000"), "limit 0");
   CHECK_MSG(!request(id + "00" "00" "00" "15" "0000"), "limit 21");
   CHECK_MSG(request(id + "00" "00" "00" "01" "2000" + Repeat("31", 32)), "since of 32 bytes");
   CHECK_MSG(!request(id + "00" "00" "00" "01" "2100" + Repeat("31", 33)), "since of 33 bytes");
   CHECK_MSG(!request(id + "00" "00" "00" "0a"), "no since");
   // Encode refuses the same.
   CHECK(Wire::Encode(Wire::TopScoresRequest { 1, 0, 0, 0, 0, "" }).empty());
   CHECK(Wire::Encode(Wire::TopScoresRequest { 1, 3, 0, 0, 1, "" }).empty());
   CHECK(Wire::Encode(Wire::TopScoresRequest { 1, 0, 0, 0, 1, std::string(33, 'x') }).empty());

   const auto scores = [](const std::string& hex) { Wire::TopScores out; return Wire::Decode(ScorbitTest::Unhex(hex), out); };
   const std::string entry = "0100" "0100000000000000" "0100" "41" "0100" "41" "00" "0000";
   CHECK(scores(id + "00" "00000000" "01" + entry));
   CHECK_MSG(scores(id + "04" "00000000" "00"), "superseded, no entries");
   CHECK_MSG(!scores(id + "05" "00000000" "00"), "result 5");
   CHECK_MSG(!scores(id + "03" "00000000" "01" + entry), "entries with a failure");
   CHECK_MSG(!scores(id + "00" "00000000" "15" + Repeat(entry.c_str(), 21)), "21 entries");
   CHECK_MSG(scores(id + "00" "00000000" "14" + Repeat(entry.c_str(), 20)), "20 entries");
   CHECK_MSG(!scores(id + "00" "00000000" "02" + entry), "fewer entries than the count");
   CHECK_MSG(!scores(id + "00" "00000000" "01" "0100" "0100000000000000" "4100" + Repeat("41", 65) + "0100" "41" "00" "0000"),
      "display_name of 65 bytes");
   CHECK_MSG(!scores(id + "00" "00000000" "01" "0100" "0100000000000000" "0100" "41" "0900" + Repeat("41", 9) + "00" "0000"),
      "initials of 9 bytes");
   CHECK_MSG(!scores(id + "00" "00000000" "01" "0100" "0100000000000000" "0100" "41" "0100" "41" "00" "2100" + Repeat("31", 33)),
      "created of 33 bytes");
   // A score past 2^32 and a negative one survive as i64.
   Wire::TopScores big { 2, 0, 7, { { 1, 0x123456789ALL, "x", "", 0, "" }, { 2, -1, "y", "", 0, "" } } };
   Wire::TopScores back;
   CHECK(Wire::Decode(Wire::Encode(big), back) && back == big);

   const auto players = [](const std::string& hex) { Wire::Players out; return Wire::Decode(ScorbitTest::Unhex(hex), out); };
   const auto slot = [](const char* player) { return std::string(player) + "0000" "0000" "0000" "0000" "0000"; };
   CHECK(players("09" + slot("01") + slot("02") + slot("03") + slot("04") + slot("05") + slot("06") + slot("07") + slot("08") + slot("09")));
   CHECK_MSG(!players("0a" + Repeat(slot("01").c_str(), 10)), "count 10");
   CHECK_MSG(!players("01" + slot("00")), "player 0");
   CHECK_MSG(!players("01" + slot("0a")), "player 10");
   CHECK_MSG(!players("02" + slot("02") + slot("01")), "descending");
   CHECK_MSG(!players("02" + slot("01") + slot("01")), "a player twice");
   CHECK_MSG(!players("02" + slot("01")), "fewer slots than the count");
   CHECK_MSG(!players("01" "01" "0000" "4100" + Repeat("41", 65) + "0000" "0000" "0000"), "preferred_name of 65 bytes");
   CHECK_MSG(!players("01" "01" "0000" "0000" "4100" + Repeat("41", 65) + "0000" "0000"), "name of 65 bytes");
   CHECK_MSG(!players("01" "01" "0000" "0000" "0000" "0900" + Repeat("41", 9) + "0000"), "initials of 9 bytes");
   CHECK_MSG(players("01" "01" "0000" "0000" "0000" "0000" "0002" + Repeat("41", 512)), "deeplink of 512 bytes");
   CHECK_MSG(!players("01" "01" "0000" "0000" "0000" "0000" "0102" + Repeat("41", 513)), "deeplink of 513 bytes");
   Wire::PlayerSlot second, first;
   second.player = 2;
   first.player = 1;
   CHECK_MSG(Wire::Encode(Wire::Players { { second, first } }).empty(), "Encode refuses slots out of order");

   const auto picture = [](const std::string& hex) { Wire::PlayerPicture out; return Wire::Decode(ScorbitTest::Unhex(hex), out); };
   CHECK(picture("09" "0100" "70" "0000"));
   CHECK_MSG(!picture("00" "0100" "70" "0000"), "player 0");
   CHECK_MSG(!picture("0a" "0100" "70" "0000"), "player 10");
   CHECK_MSG(!picture("01" "0100" "70"), "no path");
   CHECK(Wire::Encode(Wire::PlayerPicture { 0, "p", "" }).empty());

   // What a wire dump blanks of a received request: deeplinks and paths, and all of one that does not decode.
   Wire::Message msg;
   msg.header.type = Wire::TYPE_PLAYER_PICTURE;
   msg.payload = ScorbitTest::Unhex("01" "0100" "70" "0300" "2f612f");
   std::vector<Wire::SensitiveSpan> spans = Wire::ReceivedSensitive(msg);
   CHECK(spans.size() == 1 && spans[0].offset == 6 && spans[0].length == 3);
   msg.payload = ScorbitTest::Unhex("01" "0100" "70" "0300" "2f61");
   spans = Wire::ReceivedSensitive(msg);
   CHECK_MSG(spans.size() == 1 && spans[0].offset == 0 && spans[0].length == msg.payload.size(), "undecodable, blanked whole");
   msg.header.type = Wire::TYPE_STATUS;
   CHECK(Wire::ReceivedSensitive(msg).empty());
   msg.header.type = Wire::TYPE_PLAYERS;
   msg.header.flags = Wire::FLAG_RESPONSE;
   CHECK_MSG(Wire::ReceivedSensitive(msg).empty(), "responses carry none");
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
   TestTableFileName();
   TestSliceTwoGoldenVectors();
   TestSliceTwoRoundTripsAndTruncation();
   TestWriteGoldenVectors();
   TestWriteRoundTripsAndTruncation();
   TestSliceFourGoldenVectors();
   TestSliceFourTruncation();
   TestSliceFiveGoldenVectors();
   TestGameRejections();
   TestSliceSixGoldenVectors();
   TestSliceSixRejections();
   return ScorbitTest::Summary("wire_codec_test");
}
