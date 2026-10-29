// license:GPLv3+

// The authored table's host-side pieces (SB-5990): what Scorbit.Declare accepts, how VPX's
// native paths become UTF-8, and that a dump never holds the bytes a sender marks sensitive.

#include "AuthoredTable.h"
#include "GameQueue.h"
#include "WireDump.h"

#include "Check.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace Scorbit;

namespace
{

void TestMachineIds()
{
   const std::string canonical = "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90";
   std::string out;
   CHECK(CanonicalMachineId(canonical, out) && out == canonical);
   out.clear();
   CHECK_MSG(CanonicalMachineId("0B6C2F6E-7D1A-4C3E-9F20-5A8E1D4B7C90", out) && out == canonical, "upper case is lowered");
   CHECK_MSG(CanonicalMachineId("0b6C2f6E-7d1A-4c3E-9f20-5A8e1D4b7C90", out) && out == canonical, "mixed case is lowered");

   const char* refused[] = {
      "",
      "1582",
      "0b6c2f6e7d1a4c3e9f205a8e1d4b7c90",         // no dashes
      "{0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90}",   // braces
      "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c9",      // short
      "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c900",    // long
      "0b6c2f6e-7d1a-4c3e-9f20_5a8e1d4b7c90",     // a dash elsewhere
      "0b6c2f6e-7d1a-4c3e-9f2-05a8e1d4b7c90",     // a dash moved
      "0b6c2f6g-7d1a-4c3e-9f20-5a8e1d4b7c90",     // not hex
      " b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c90",     // a space
      "0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7c9\n",    // a control character
   };
   for (const char* id : refused)
   {
      out = "untouched";
      CHECK_MSG(!CanonicalMachineId(id, out) && out == "untouched", id);
   }
   CHECK_MSG(!CanonicalMachineId("0b6c2f6e-7d1a-4c3e-9f20-5a8e1d4b7\xc3\xa9", out), "non-ASCII");
}

void TestNativePaths()
{
   CHECK(NativePathToUtf8(nullptr).empty());
   CHECK(NativePathToUtf8("").empty());
   CHECK(NativePathToUtf8("/tables/Meteor.vpx") == "/tables/Meteor.vpx");
#ifndef _WIN32
   CHECK_MSG(NativePathToUtf8("/tables/caf\xc3\xa9.vpx") == "/tables/caf\xc3\xa9.vpx", "UTF-8 passes through");
#endif
}

void SetDumpPath(const std::string& path)
{
#ifdef _WIN32
   _putenv_s("SCORBIT_WIRE_DUMP", path.c_str());
#else
   setenv("SCORBIT_WIRE_DUMP", path.c_str(), 1);
#endif
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// The frame part of the only record in a dump: past the 13-byte magic and the 13-byte record header.
std::vector<uint8_t> OnlyFrame(const std::vector<uint8_t>& dump)
{
   constexpr size_t AT = 13 + 13;
   return dump.size() > AT ? std::vector<uint8_t>(dump.begin() + AT, dump.end()) : std::vector<uint8_t> { };
}

std::vector<uint8_t> DumpOne(const std::vector<uint8_t>& frame, const std::vector<SensitiveSpan>& spans)
{
   const std::filesystem::path path = std::filesystem::temp_directory_path() / "scorbit-authored-table-test.wire";
   SetDumpPath(path.string());
   {
      WireDump dump;
      std::string error;
      dump.Open(error);
      CHECK_MSG(error.empty() && dump.Enabled(), "the dump opens");
      dump.Sent(frame.data(), frame.size(), spans);
   }
   std::vector<uint8_t> written = OnlyFrame(ReadAll(path));
   std::filesystem::remove(path);
   return written;
}

void TestDumpRedactsSensitiveSpans()
{
   const std::string text = "id=1582;path=/Users/someone/tables/Original.vpx;end";
   const std::vector<uint8_t> frame(text.begin(), text.end());
   const size_t at = text.find("/Users");
   const size_t length = text.find(";end") - at;

   const std::vector<uint8_t> written = DumpOne(frame, { { at, length } });
   const std::string expected = "id=1582;path=" + std::string(length, '*') + ";end";
   CHECK_MSG(written == std::vector<uint8_t>(expected.begin(), expected.end()), "the path is blanked at its own length");

   CHECK_MSG(DumpOne(frame, { }) == frame, "no spans, the frame as sent");
   const std::vector<uint8_t> clipped = DumpOne(frame, { { frame.size() - 3, 100 }, { frame.size() + 5, 2 } });
   CHECK_MSG(clipped.size() == frame.size(), "a span past the end is clipped, never written past");
   CHECK(std::string(clipped.end() - 3, clipped.end()) == "***");
}


GamePost Post(uint8_t first, size_t ops)
{
   GamePost post;
   post.epoch = 1;
   Wire::GameOp op;
   op.kind = first;
   post.batch.ops.push_back(op);
   op.kind = Wire::GAME_CLEAR_MODES;
   post.batch.ops.resize(ops, op);
   return post;
}

std::vector<GamePost> Posts(std::initializer_list<std::pair<uint8_t, size_t>> list)
{
   std::vector<GamePost> out;
   for (const auto& [first, ops] : list)
      out.push_back(Post(first, ops));
   return out;
}

std::vector<size_t> Sizes(GameQueue& q)
{
   std::deque<GamePost> taken;
   q.TakeAll(taken);
   std::vector<size_t> sizes;
   for (const GamePost& p : taken)
      sizes.push_back(p.batch.ops.size());
   return sizes;
}

// While no daemon takes anything the queue fills; a new game then evicts the oldest whole
// game rather than being refused, so the newest games are the ones kept.
void TestGameQueueKeepsTheNewestGames()
{
   GameQueue q(10);
   CHECK(!q.Push(Posts({ { Wire::GAME_START, 3 }, { Wire::GAME_CLEAR_MODES, 2 } })));
   CHECK(!q.Push(Posts({ { Wire::GAME_START, 5 } })));
   CHECK(q.QueuedOps() == 10);
   CHECK_MSG(q.Push(Posts({ { Wire::GAME_START, 4 } })), "a drop is reported");
   CHECK_MSG(Sizes(q) == (std::vector<size_t> { 5, 4 }), "the oldest game, start and tail, went first");
   CHECK(q.QueuedOps() == 0);
}

// One game alone outgrows the queue: its later batches are refused until a new start, which
// may then evict it whole.
void TestGameQueueRefusesAnOutgrownGame()
{
   GameQueue q(6);
   CHECK(!q.Push(Posts({ { Wire::GAME_START, 3 } })));
   CHECK(q.Push(Posts({ { Wire::GAME_CLEAR_MODES, 4 } })));
   CHECK_MSG(q.Push(Posts({ { Wire::GAME_CLEAR_MODES, 1 } })), "the rest of that game is refused, though it would fit");
   CHECK(q.QueuedOps() == 3);
   CHECK_MSG(q.Push(Posts({ { Wire::GAME_START, 5 } })), "the new game evicts the old one");
   CHECK(Sizes(q) == std::vector<size_t> { 5 });
   CHECK_MSG(!q.Push(Posts({ { Wire::GAME_CLEAR_MODES, 1 } })), "and the new game's tail is taken again");
   CHECK(Sizes(q) == std::vector<size_t> { 1 });
}
}

int main()
{
   TestMachineIds();
   TestNativePaths();
   TestDumpRedactsSensitiveSpans();
   TestGameQueueKeepsTheNewestGames();
   TestGameQueueRefusesAnOutgrownGame();
   return ScorbitTest::Summary("authored_table_test");
}
