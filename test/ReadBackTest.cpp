// license:GPLv3+

// The script's read-backs without a socket: the channel's bounds, the result store the script
// reads by request id, and the worker's slot table with its picture copies.

#include "ReadBack.h"

#include "Check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifndef _WIN32
   #include <sys/stat.h>
#endif

using namespace Scorbit;
namespace fs = std::filesystem;

namespace
{

using Clock = std::chrono::steady_clock;

TopScoresPost Result(uint64_t epoch, uint32_t id, int result, size_t entries = 0, uint32_t ageS = 0)
{
   TopScoresPost post;
   post.epoch = epoch;
   post.requestId = id;
   post.result = result;
   post.ageS = ageS;
   for (size_t i = 0; i < entries; i++)
      post.entries.push_back({ static_cast<uint16_t>(i + 1), 1000 - static_cast<int64_t>(i), "P" + std::to_string(i), "I", 1, "c" });
   return post;
}

void TestChannelRequests()
{
   ReadBackChannel channel;
   CHECK_MSG(channel.Request(1, { }) == 0, "no id while leaderboards are unavailable");
   channel.SetLeaderboards(true);
   const uint32_t first = channel.Request(1, { });
   const uint32_t second = channel.Request(1, { });
   CHECK(first == 1 && second == 2);
   for (size_t i = 2; i < ReadBackChannel::MAX_QUEUED; i++)
      CHECK(channel.Request(1, { }) != 0);
   CHECK_MSG(channel.Request(1, { }) == 0, "past the bound, refused rather than dropped");
   std::deque<TopScoresAsk> asks;
   channel.TakeRequests(asks);
   CHECK(asks.size() == ReadBackChannel::MAX_QUEUED && asks.front().request.requestId == 1 && asks.front().epoch == 1);
   CHECK_MSG(channel.Request(2, { }) == ReadBackChannel::MAX_QUEUED + 1, "ids keep counting");

   for (uint32_t i = 1; i <= ReadBackChannel::MAX_QUEUED + 3; i++)
      channel.PostResult(Result(1, i, 0));
   std::deque<TopScoresPost> results;
   channel.TakeResults(results);
   CHECK_MSG(results.size() == ReadBackChannel::MAX_QUEUED && results.front().requestId == 4, "the newest results are kept");
}

void TestStoreKeepsTheEightMostRecent()
{
   TopScoresStore store;
   const Clock::time_point now = Clock::now();
   for (uint32_t id = 1; id <= TopScoresStore::CAPACITY + 2; id++)
      store.Add(Result(1, id, 0), now);
   CHECK(store.Find(1) == nullptr && store.Find(2) == nullptr);
   for (uint32_t id = 3; id <= TopScoresStore::CAPACITY + 2; id++)
      CHECK_MSG(store.Find(id) != nullptr, "id " + std::to_string(id));
   store.Release(5);
   CHECK(store.Find(5) == nullptr && store.Find(6) != nullptr);
   store.Add(Result(1, 6, 3), now);
   CHECK_MSG(store.Find(6) != nullptr && store.Find(6)->post.result == 3, "the same id replaces its result");
   store.Clear();
   CHECK(store.Find(6) == nullptr);
}

void TestStateReadsByRequestId()
{
   ReadBackChannel channel;
   ReadBackState state;
   state.NewEpoch(3);
   const Clock::time_point t0 = Clock::now();
   channel.PostResult(Result(3, 7, Wire::TOP_SCORES_OK, 2, 10));
   channel.PostResult(Result(3, 8, SCORES_NOT_DECLARED));
   channel.PostResult(Result(2, 9, Wire::TOP_SCORES_OK, 1));
   state.Drain(channel, t0);

   CHECK(state.Result(7) == Wire::TOP_SCORES_OK && state.Count(7) == 2);
   CHECK(state.Result(8) == SCORES_NOT_DECLARED && state.Count(8) == 0);
   CHECK_MSG(state.Result(9) == SCORES_NONE, "a result from another table is dropped");
   CHECK(state.Result(1234) == SCORES_NONE && state.Count(1234) == 0 && state.Age(1234, t0) == 0);
   CHECK(state.Age(7, t0) == 10 && state.Age(7, t0 + std::chrono::seconds(5)) == 15);

   const Wire::TopScoreEntry* e = state.Score(7, 1);
   CHECK(e != nullptr && e->rank == 2 && e->highScore == 999 && e->displayName == "P1");
   CHECK(state.Score(7, 0) != nullptr);
   CHECK_MSG(state.Score(7, 2) == nullptr && state.Score(7, -1) == nullptr && state.Score(7, 0.5) == nullptr,
      "out of range, negative and fractional indexes read nothing");
   CHECK(state.Score(8, 0) == nullptr);
   CHECK(state.Result(7.5) == SCORES_NONE && state.Result(-7) == SCORES_NONE && state.Result(1e300) == SCORES_NONE);

   state.Release(7);
   CHECK(state.Result(7) == SCORES_NONE && state.Result(8) == SCORES_NOT_DECLARED);
   state.NewEpoch(4);
   CHECK_MSG(state.Result(8) == SCORES_NONE, "a table switch clears the results");
}

void TestStatePlayers()
{
   ReadBackChannel channel;
   ReadBackState state;
   state.NewEpoch(1);
   CHECK(state.Slot(1) != nullptr && !state.Slot(1)->Claimed());
   CHECK(state.Slot(0) == nullptr && state.Slot(10) == nullptr && state.Slot(1.5) == nullptr);

   PlayersPost post;
   post.epoch = 1;
   post.slots[0].playerId = "p-1";
   post.slots[0].name = "Ace Player";
   post.slots[8].claimDeeplink = "link";
   channel.PostPlayers(post);
   state.Drain(channel, Clock::now());
   CHECK(state.Slot(1)->Claimed() && state.Slot(1)->DisplayName() == "Ace Player" && state.Slot(9)->claimDeeplink == "link");

   post.epoch = 0;
   post.slots[0].playerId = "p-old";
   channel.PostPlayers(post);
   state.Drain(channel, Clock::now());
   CHECK_MSG(state.Slot(1)->playerId == "p-1", "players from another table are dropped");

   state.NewEpoch(2);
   CHECK(!state.Slot(1)->Claimed() && state.Slot(9)->claimDeeplink.empty());
}

void Write(const fs::path& path, const std::string& bytes)
{
   std::ofstream out(path, std::ios::binary);
   out << bytes;
}

std::string Read(const fs::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::string Utf8(const fs::path& p)
{
   const std::u8string u = p.u8string();
   return std::string(u.begin(), u.end());
}

void TestPlayerTable()
{
   std::error_code ec;
   const fs::path root = fs::temp_directory_path(ec) / "scorbit-read-back-test";
   fs::remove_all(root, ec);
   fs::create_directories(root, ec);
   const fs::path dir = root / "pictures";
   const fs::path a = root / "a.jpg";
   const fs::path b = root / "b.jpg";
   const fs::path big = root / "big.jpg";
   Write(a, "picture a");
   Write(b, "picture b");
   Write(big, std::string(PlayerTable::MAX_PICTURE_BYTES + 1, 'x'));

   {
      PlayerTable table;
      table.SetDirectory(dir, 0x2a);
      Wire::Players players { { { 1, "p-1", "Ace", "Ace Player", "ACE", "" }, { 3, "", "", "", "", "link" } } };
      CHECK(table.Apply(players));
      CHECK_MSG(!table.Apply(players), "the same table changes nothing");

      std::string error;
      CHECK(table.Apply(Wire::PlayerPicture { 3, "", Utf8(a) }, error) == PlayerTable::Picture::Ignored);
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-2", Utf8(a) }, error) == PlayerTable::Picture::Ignored);
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Stored);
      PlayersPost post;
      table.Fill(post);
      const fs::path copy = post.slots[0].picture;
      CHECK(copy.parent_path() == dir && Read(copy) == "picture a" && copy != a);
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Unchanged);

      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(big) }, error) == PlayerTable::Picture::Failed);
      CHECK(!error.empty() && error.find(root.string()) == std::string::npos);
      table.Fill(post);
      CHECK_MSG(post.slots[0].picture.empty() && !fs::exists(copy), "a failed copy leaves no picture, old or new");
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(root / "none.jpg") }, error) == PlayerTable::Picture::Failed);

      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(b) }, error) == PlayerTable::Picture::Stored);
      table.Fill(post);
      const fs::path copyB = post.slots[0].picture;
      CHECK(Read(copyB) == "picture b");
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", "" }, error) == PlayerTable::Picture::Removed);
      CHECK(!fs::exists(copyB));
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", "" }, error) == PlayerTable::Picture::Unchanged);

      // A slot left out of the next table is empty, and its picture goes.
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Stored);
      table.Fill(post);
      const fs::path copyA = post.slots[0].picture;
      CHECK(table.Apply(Wire::Players { { { 3, "", "", "", "", "link" } } }));
      table.Fill(post);
      CHECK(!post.slots[0].Claimed() && post.slots[0].picture.empty() && !fs::exists(copyA));

      CHECK(table.Apply(players));
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Stored);
      table.Fill(post);
      const fs::path last = post.slots[0].picture;
      CHECK(table.Clear() && !fs::exists(last));
      CHECK(!table.Clear());

      CHECK(table.Apply(players));
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Stored);
   }
   CHECK_MSG(fs::is_empty(dir, ec), "the destructor deletes what it held");

   {
      PlayerTable none;
      none.SetDirectory({ }, 1);
      none.Apply(Wire::Players { { { 1, "p-1", "", "", "", "" } } });
      std::string error;
      CHECK_MSG(none.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Failed && !error.empty(),
         "no directory fails, never throws");
   }
   fs::remove_all(root, ec);
}

size_t FilesIn(const fs::path& dir)
{
   std::error_code ec;
   size_t n = 0;
   for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
      n++;
   return n;
}

// Closing admits nothing more and returns everything admitted, so no id is left unanswered.
void TestChannelClose()
{
   ReadBackChannel channel;
   channel.SetLeaderboards(true);
   const uint32_t a = channel.Request(1, { });
   const uint32_t b = channel.Request(1, { });
   std::deque<TopScoresAsk> closed;
   channel.Close(closed);
   CHECK(!channel.Leaderboards());
   CHECK(closed.size() == 2 && closed.size() == 2 && closed[0].request.requestId == a && closed[1].request.requestId == b);
   CHECK_MSG(channel.Request(1, { }) == 0, "nothing is admitted after Close");
   std::deque<TopScoresAsk> left;
   channel.TakeRequests(left);
   CHECK(left.empty());
}

// The age counts from the worker's receipt, not from when the script first read it.
void TestAgeCountsFromReceipt()
{
   ReadBackChannel channel;
   ReadBackState state;
   state.NewEpoch(1);
   const Clock::time_point t0 = Clock::now();
   TopScoresPost post = Result(1, 5, Wire::TOP_SCORES_OK, 1, 3);
   post.received = t0 - std::chrono::seconds(20);
   channel.PostResult(std::move(post));
   state.Drain(channel, t0);
   CHECK_MSG(state.Age(5, t0) == 23, "age " + std::to_string(state.Age(5, t0)));
}

#ifndef _WIN32
mode_t Mode(const fs::path& p)
{
   struct stat st { };
   return ::lstat(p.c_str(), &st) == 0 ? st.st_mode : 0;
}

// The picture directory is created 0700, and an existing one is used only when it is this user's alone.
void TestPictureDirectoryOwnership()
{
   std::error_code ec;
   const fs::path root = fs::temp_directory_path(ec) / "scorbit-read-back-dirs";
   fs::remove_all(root, ec);
   fs::create_directories(root, ec);
   const fs::path a = root / "a.jpg";
   Write(a, "picture a");
   const Wire::Players claimed { { { 1, "p-1", "", "", "", "" } } };
   std::string error;

   {
      const fs::path dir = root / "fresh" / "scorbit-vpx" / "pictures";
      PlayerTable table;
      table.SetDirectory(dir, 1);
      table.Apply(claimed);
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Stored);
      CHECK_MSG((Mode(dir) & 0777) == 0700 && (Mode(dir.parent_path()) & 0777) == 0700, "created owner-only");
   }
   {
      const fs::path dir = root / "shared" / "scorbit-vpx" / "pictures";
      fs::create_directories(dir, ec);
      fs::permissions(dir, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::replace, ec);
      PlayerTable table;
      table.SetDirectory(dir, 1);
      table.Apply(claimed);
      CHECK(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Failed);
      CHECK_MSG(table.DirectoryRefused() && !error.empty() && error.find(root.string()) == std::string::npos, error);
      CHECK_MSG(FilesIn(dir) == 0, "nothing is copied into a writable-by-others directory");
      fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
      CHECK_MSG(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Failed,
         "a refusal holds for the load");
   }
   {
      const fs::path real = root / "elsewhere";
      fs::create_directories(real, ec);
      fs::permissions(real, fs::perms::owner_all, fs::perm_options::replace, ec);
      const fs::path parent = root / "linked" / "scorbit-vpx";
      fs::create_directories(parent, ec);
      fs::create_directory_symlink(real, parent / "pictures", ec);
      PlayerTable table;
      table.SetDirectory(parent / "pictures", 1);
      table.Apply(claimed);
      CHECK_MSG(table.Apply(Wire::PlayerPicture { 1, "p-1", Utf8(a) }, error) == PlayerTable::Picture::Failed
         && table.DirectoryRefused() && FilesIn(real) == 0, "a symlink is not taken for the directory");
   }
   fs::remove_all(root, ec);
}

// Never the shared temp directory.
void TestDefaultPictureDirectory()
{
   const auto restore = [](const char* name, const char* value)
   {
      if (value != nullptr)
         setenv(name, value, 1);
      else
         unsetenv(name);
   };
#if defined(__APPLE__)
   const std::string tmp = getenv("TMPDIR") != nullptr ? getenv("TMPDIR") : "";
   setenv("TMPDIR", "/Users/someone/T", 1);
   CHECK(DefaultPictureDirectory() == fs::path("/Users/someone/T/scorbit-vpx/pictures"));
   restore("TMPDIR", tmp.empty() ? nullptr : tmp.c_str());
#else
   const std::string run = getenv("XDG_RUNTIME_DIR") != nullptr ? getenv("XDG_RUNTIME_DIR") : "";
   const std::string cache = getenv("XDG_CACHE_HOME") != nullptr ? getenv("XDG_CACHE_HOME") : "";
   const std::string home = getenv("HOME") != nullptr ? getenv("HOME") : "";
   setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
   CHECK(DefaultPictureDirectory() == fs::path("/run/user/1000/scorbit-vpx/pictures"));
   unsetenv("XDG_RUNTIME_DIR");
   setenv("XDG_CACHE_HOME", "/home/someone/.cachex", 1);
   CHECK(DefaultPictureDirectory() == fs::path("/home/someone/.cachex/scorbit-vpx/pictures"));
   unsetenv("XDG_CACHE_HOME");
   setenv("HOME", "/home/someone", 1);
   CHECK(DefaultPictureDirectory() == fs::path("/home/someone/.cache/scorbit-vpx/pictures"));
   restore("XDG_RUNTIME_DIR", run.empty() ? nullptr : run.c_str());
   restore("XDG_CACHE_HOME", cache.empty() ? nullptr : cache.c_str());
   restore("HOME", home.empty() ? nullptr : home.c_str());
#endif
   CHECK(DefaultPictureDirectory().string().rfind("/tmp/", 0) != 0);
}
#endif

void TestDisplayName()
{
   PlayerView v;
   v.name = "Ace Player";
   CHECK(v.DisplayName() == "Ace Player");
   v.preferredName = "ACE";
   CHECK(v.DisplayName() == "ACE");
}

}

int main()
{
   TestChannelRequests();
   TestStoreKeepsTheEightMostRecent();
   TestStateReadsByRequestId();
   TestStatePlayers();
   TestPlayerTable();
   TestDisplayName();
   TestChannelClose();
   TestAgeCountsFromReceipt();
#ifndef _WIN32
   TestPictureDirectoryOwnership();
   TestDefaultPictureDirectory();
#endif
   return ScorbitTest::Summary("read_back_test");
}
