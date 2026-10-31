// license:GPLv3+

#include "ReadBack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
   #define WIN32_LEAN_AND_MEAN
   #define NOMINMAX
   #include <windows.h>
#else
   #include <cerrno>
   #include <csignal>
   #include <pwd.h>
   #include <sys/stat.h>
   #include <unistd.h>
#endif

namespace Scorbit
{

namespace fs = std::filesystem;

namespace
{

// Wire strings are UTF-8 on every platform; a narrow path on Windows would be the code page.
fs::path FromUtf8(const std::string& s)
{
   return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string ToUtf8(const fs::path& p)
{
   const std::u8string u = p.u8string();
   return std::string(u.begin(), u.end());
}

unsigned long ThisProcess()
{
#ifdef _WIN32
   return GetCurrentProcessId();
#else
   return static_cast<unsigned long>(getpid());
#endif
}

// Errs towards alive: a copy kept by mistake costs a file, one deleted by mistake a picture.
bool ProcessAlive(unsigned long pid)
{
#ifdef _WIN32
   const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
   if (h == nullptr)
      return GetLastError() != ERROR_INVALID_PARAMETER;
   DWORD code = 0;
   const bool alive = !GetExitCodeProcess(h, &code) || code == STILL_ACTIVE;
   CloseHandle(h);
   return alive;
#else
   return pid == 0 || pid > 0x7FFFFFFFul || kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

// A copy's name: "<pid>-<tag>-<player>-<n>.jpg", the pid being the process that made it.
bool ParseCopyName(const std::string& name, unsigned long& pid)
{
   unsigned long long tag = 0, n = 0;
   unsigned player = 0;
   int used = 0;
   return std::sscanf(name.c_str(), "%lu-%16llx-%u-%llu.jpg%n", &pid, &tag, &player, &n, &used) == 4
      && used == static_cast<int>(name.size());
}

// Both sides are small enough to read whole; a read failure counts as different.
bool SameBytes(const fs::path& a, const fs::path& b)
{
   std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
   if (!fa || !fb)
      return false;
   return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
      std::istreambuf_iterator<char>(fb), std::istreambuf_iterator<char>());
}

}

uint32_t ReadBackChannel::Request(uint64_t epoch, Wire::TopScoresRequest request, uint64_t identity)
{
   std::lock_guard lock(m_mutex);
   if (!m_leaderboards || m_requests.size() >= MAX_QUEUED)
      return 0;
   request.requestId = m_nextId;
   m_nextId = m_nextId >= 0x7FFFFFFFu ? 1 : m_nextId + 1;
   m_requests.push_back({ epoch, identity, std::move(request) });
   return m_requests.back().request.requestId;
}

void ReadBackChannel::TakeRequests(std::deque<TopScoresAsk>& out)
{
   std::lock_guard lock(m_mutex);
   for (TopScoresAsk& ask : m_requests)
      out.push_back(std::move(ask));
   m_requests.clear();
}

void ReadBackChannel::SetLeaderboards(bool available)
{
   std::lock_guard lock(m_mutex);
   m_leaderboards = available;
}

bool ReadBackChannel::Leaderboards() const
{
   std::lock_guard lock(m_mutex);
   return m_leaderboards;
}

void ReadBackChannel::Close(std::deque<TopScoresAsk>& out)
{
   std::lock_guard lock(m_mutex);
   m_leaderboards = false;
   for (TopScoresAsk& ask : m_requests)
      out.push_back(std::move(ask));
   m_requests.clear();
}

void ReadBackChannel::PostResult(TopScoresPost post)
{
   std::lock_guard lock(m_mutex);
   if (m_results.size() >= MAX_QUEUED)
      m_results.pop_front();
   m_results.push_back(std::move(post));
}

void ReadBackChannel::TakeResults(std::deque<TopScoresPost>& out)
{
   std::lock_guard lock(m_mutex);
   for (TopScoresPost& post : m_results)
      out.push_back(std::move(post));
   m_results.clear();
}

fs::path DefaultPictureDirectory()
{
#if defined(_WIN32)
   // The temp directory is per user on Windows, so it is a safe second choice there.
   if (const wchar_t* base = _wgetenv(L"LOCALAPPDATA"); base != nullptr && base[0] != L'\0')
      return fs::path(base) / L"Scorbit" / L"vpx-pictures";
   std::error_code ec;
   const fs::path tmp = fs::temp_directory_path(ec);
   return ec || tmp.empty() ? fs::path() : tmp / L"Scorbit" / L"vpx-pictures";
#else
   // Never the shared /tmp: another user could own the directory, or read what lands in it.
   const auto env = [](const char* name) -> fs::path
   {
      const char* v = getenv(name);
      return v != nullptr && v[0] != '\0' ? fs::path(v) : fs::path();
   };
   fs::path base;
   #if defined(__APPLE__)
   base = env("TMPDIR");
   if (base.empty())
   {
      char buf[1024] = { };
      const size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf));
      if (n != 0 && n <= sizeof(buf))
         base = buf;
   }
   #else
   base = env("XDG_RUNTIME_DIR");
   if (base.empty())
      base = env("XDG_CACHE_HOME");
   if (base.empty())
   {
      fs::path home = env("HOME");
      if (home.empty())
         if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] != '\0')
            home = pw->pw_dir;
      if (!home.empty())
         base = home / ".cache";
   }
   #endif
   return base.empty() ? fs::path() : base / "scorbit-vpx" / "pictures";
#endif
}

bool PlayerTable::PrepareDirectory(std::string& error)
{
   if (m_dir.empty())
   {
      error = "no directory for player pictures on this platform";
      return false;
   }
   if (m_refused)
   {
      error = "the picture directory is not this user's alone";
      return false;
   }
   std::error_code ec;
#ifdef _WIN32
   fs::create_directories(m_dir, ec);
   if (ec)
   {
      error = "cannot create the picture directory (" + ec.message() + ')';
      return false;
   }
   SweepOnce();
   return true;
#else
   // The leaf and the scorbit-vpx above it are made 0700 here; whatever already exists must be a
   // real directory of this uid that no one else can write, or a stranger could swap files in.
   const fs::path parent = m_dir.parent_path();
   fs::create_directories(parent.parent_path(), ec);
   for (const fs::path& dir : { parent, m_dir })
   {
      if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
      {
         error = "cannot create the picture directory (" + std::error_code(errno, std::generic_category()).message() + ')';
         return false;
      }
      struct stat st { };
      if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
      {
         m_refused = true;
         error = "the picture directory is not this user's alone";
         return false;
      }
   }
   SweepOnce();
   return true;
#endif
}

void PlayerTable::SweepOnce()
{
   if (m_swept)
      return;
   m_swept = true;
   // Copies a process left behind when it ended without unloading; a live process's are its own.
   std::error_code ec;
   bool failed = false;
   for (fs::directory_iterator it(m_dir, ec), end; !ec && it != end; it.increment(ec))
   {
      std::error_code fe;
      const fs::file_status status = it->symlink_status(fe);
      unsigned long pid = 0;
      if (fe || !fs::is_regular_file(status) || !ParseCopyName(ToUtf8(it->path().filename()), pid)
         || pid == ThisProcess() || ProcessAlive(pid))
         continue;
      if (!fs::remove(it->path(), fe) && fe)
         failed = true;
   }
   if (ec || failed)
      m_sweepError = "cannot remove pictures left by an earlier run (" + (ec ? ec : std::make_error_code(std::errc::io_error)).message() + ')';
}

void PlayerTable::SetDirectory(fs::path dir, uint64_t tag)
{
   m_dir = std::move(dir);
   m_tag = tag;
}

void PlayerTable::DropPicture(Slot& slot)
{
   if (!slot.copy.empty())
   {
      std::error_code ec;
      fs::remove(slot.copy, ec);
   }
   slot.copy.clear();
   slot.source.clear();
   slot.view.picture.clear();
}

bool PlayerTable::Apply(const Wire::Players& players)
{
   bool changed = false;
   for (size_t i = 0; i < m_slots.size(); i++)
   {
      PlayerView next;
      for (const Wire::PlayerSlot& s : players.slots)
         if (s.player == i + 1)
         {
            next.playerId = s.playerId;
            next.preferredName = s.preferredName;
            next.name = s.name;
            next.initials = s.initials;
            next.claimDeeplink = s.claimDeeplink;
         }
      Slot& slot = m_slots[i];
      // A picture belongs to its claim: an empty slot, or someone else's, supersedes it.
      if (!next.Claimed() || next.playerId != slot.view.playerId)
         DropPicture(slot);
      next.picture = slot.view.picture;
      if (!(next == slot.view))
      {
         slot.view = std::move(next);
         changed = true;
      }
   }
   return changed;
}

PlayerTable::Picture PlayerTable::Apply(const Wire::PlayerPicture& picture, std::string& error)
{
   error.clear();
   if (picture.player < 1 || picture.player > m_slots.size())
      return Picture::Ignored;
   Slot& slot = m_slots[picture.player - 1];
   if (picture.playerId.empty() || picture.playerId != slot.view.playerId)
      return Picture::Ignored;
   if (picture.path.empty())
   {
      if (slot.copy.empty())
         return Picture::Unchanged;
      DropPicture(slot);
      return Picture::Removed;
   }
   // The daemon may delete its file after the first ack and later write a new picture at the same path.
   if (!slot.copy.empty() && picture.path == slot.source)
   {
      std::error_code ec;
      const fs::path source = FromUtf8(picture.path);
      if (!fs::exists(source, ec) || SameBytes(source, slot.copy))
         return Picture::Unchanged;
   }

   // A failure leaves the slot with no picture rather than the one this superseded.
   DropPicture(slot);
   if (!PrepareDirectory(error))
      return Picture::Failed;
   std::error_code ec;
   const fs::path source = FromUtf8(picture.path);
   if (!fs::is_regular_file(source, ec) || ec)
   {
      error = "the picture is not a readable file";
      return Picture::Failed;
   }
   const uintmax_t size = fs::file_size(source, ec);
   if (ec || size > MAX_PICTURE_BYTES)
   {
      error = ec ? "the picture's size is unreadable" : "the picture is over 2 MiB";
      return Picture::Failed;
   }
   char name[64];
   std::snprintf(name, sizeof(name), "%lu-%016llx-%u-%llu.jpg", ThisProcess(), static_cast<unsigned long long>(m_tag),
      static_cast<unsigned>(picture.player), static_cast<unsigned long long>(++m_copies));
   const fs::path copy = m_dir / name;
   if (!fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec) || ec)
   {
      error = "cannot copy the picture (" + ec.message() + ')';
      fs::remove(copy, ec);
      return Picture::Failed;
   }
   slot.copy = copy;
   slot.source = picture.path;
   slot.view.picture = ToUtf8(copy);
   return Picture::Stored;
}

bool PlayerTable::Clear()
{
   bool held = false;
   for (Slot& slot : m_slots)
   {
      held = held || !(slot.view == PlayerView { }) || !slot.copy.empty();
      DropPicture(slot);
      slot.view = { };
   }
   return held;
}

void PlayerTable::Fill(PlayersPost& out) const
{
   for (size_t i = 0; i < m_slots.size(); i++)
      out.slots[i] = m_slots[i].view;
}

void TopScoresStore::Add(TopScoresPost post, Clock::time_point now)
{
   Release(post.requestId);
   if (m_entries.size() >= CAPACITY)
      m_entries.pop_front();
   const Clock::time_point received = post.received == Clock::time_point { } ? now : post.received;
   m_entries.push_back({ std::move(post), received });
}

const TopScoresStore::Entry* TopScoresStore::Find(uint32_t requestId) const
{
   for (const Entry& e : m_entries)
      if (e.post.requestId == requestId)
         return &e;
   return nullptr;
}

void TopScoresStore::Release(uint32_t requestId)
{
   std::erase_if(m_entries, [requestId](const Entry& e) { return e.post.requestId == requestId; });
}

bool ScriptIndex(double v, double lo, double hi, uint32_t& out)
{
   if (!std::isfinite(v) || v != std::trunc(v) || v < lo || v > hi)
      return false;
   out = static_cast<uint32_t>(v);
   return true;
}

void ReadBackState::ClearScores()
{
   m_scores.Clear();
   m_clearedThrough = m_lastRequest;
}

void ReadBackState::NoteRequest(uint32_t requestId)
{
   if (requestId != 0)
      m_lastRequest = requestId;
}

void ReadBackState::NewEpoch(uint64_t epoch)
{
   m_epoch = epoch;
   m_clearedThrough = 0; // the epoch tag drops the old table's results from here
   m_scores.Clear();
   // The snapshot count belongs to the connection, which a new table does not end.
   const uint32_t snapshots = m_players.snapshots;
   m_players = { };
   m_players.epoch = epoch;
   m_players.snapshots = snapshots;
}

void ReadBackState::Drain(ReadBackChannel& channel, Clock::time_point now)
{
   if (std::unique_ptr<PlayersPost> players = channel.TakePlayers(); players && players->epoch == m_epoch)
      m_players = std::move(*players);
   std::deque<TopScoresPost> results;
   channel.TakeResults(results);
   for (TopScoresPost& post : results)
      if (post.epoch == m_epoch && (m_clearedThrough == 0 || post.requestId > m_clearedThrough))
         m_scores.Add(std::move(post), now);
}

const TopScoresStore::Entry* ReadBackState::Find(double requestId) const
{
   uint32_t id = 0;
   return ScriptIndex(requestId, 1, 0x7FFFFFFF, id) ? m_scores.Find(id) : nullptr;
}

int ReadBackState::Result(double requestId) const
{
   const TopScoresStore::Entry* e = Find(requestId);
   return e != nullptr ? e->post.result : SCORES_NONE;
}

int ReadBackState::Age(double requestId, Clock::time_point now) const
{
   const TopScoresStore::Entry* e = Find(requestId);
   if (e == nullptr)
      return 0;
   const long long held = std::chrono::duration_cast<std::chrono::seconds>(now - e->received).count();
   const long long age = static_cast<long long>(e->post.ageS) + (held > 0 ? held : 0);
   return age > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(age);
}

int ReadBackState::Count(double requestId) const
{
   const TopScoresStore::Entry* e = Find(requestId);
   return e != nullptr ? static_cast<int>(e->post.entries.size()) : 0;
}

const Wire::TopScoreEntry* ReadBackState::Score(double requestId, double index) const
{
   const TopScoresStore::Entry* e = Find(requestId);
   uint32_t i = 0;
   if (e == nullptr || e->post.entries.empty() || !ScriptIndex(index, 0, static_cast<double>(e->post.entries.size() - 1), i))
      return nullptr;
   return &e->post.entries[i];
}

void ReadBackState::Release(double requestId)
{
   uint32_t id = 0;
   if (ScriptIndex(requestId, 1, 0x7FFFFFFF, id))
      m_scores.Release(id);
}

const PlayerView* ReadBackState::Slot(double slot) const
{
   uint32_t i = 0;
   return ScriptIndex(slot, 1, Wire::MAX_PLAYERS, i) ? &m_players.slots[i - 1] : nullptr;
}

}
