// license:GPLv3+

// The authored table's host-side pieces (SB-5990): what Scorbit.Declare accepts, how VPX's
// native paths become UTF-8, and that a dump never holds the bytes a sender marks sensitive.

#include "AuthoredTable.h"
#include "WireDump.h"

#include "Check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Scorbit;

namespace
{

void TestMachineIds()
{
   CHECK(ValidMachineId("1582"));
   CHECK(ValidMachineId("a"));
   CHECK(ValidMachineId(std::string(64, 'x')));
   CHECK_MSG(!ValidMachineId(""), "empty");
   CHECK_MSG(!ValidMachineId(std::string(65, 'x')), "over 64");
   CHECK_MSG(!ValidMachineId("15 82"), "a space");
   CHECK_MSG(!ValidMachineId("1582\n"), "a control character");
   CHECK_MSG(!ValidMachineId("caf\xc3\xa9"), "non-ASCII");
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

}

int main()
{
   TestMachineIds();
   TestNativePaths();
   TestDumpRedactsSensitiveSpans();
   return ScorbitTest::Summary("authored_table_test");
}
