// license:GPLv3+

#pragma once

// The table script's read-backs (protocol 1.2): leaderboards, players and their pictures.
//
// The socket worker receives them and the API thread reads them for Scorbit.Table, so they
// cross threads here the way Status does: players through a latch holding the latest table,
// leaderboard results and the script's requests through two short, bounded queues. Neither
// thread waits on the other beyond a mutex held for a push or a swap.
//
// Everything is tagged with the Declare epoch it belongs to, so the API thread drops what
// arrives for a table it has already left.

#include "Latch.h"
#include "WireProtocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Scorbit
{

// What the script reads as TopScoresResult: the contract's 0 to 4, then the plugin's own.
enum ScriptTopScoresResult : int
{
   SCORES_NONE = -1,           // no result yet, or an id this object does not hold
   SCORES_NOT_DECLARED = 5,    // the daemon had no table declared
   SCORES_UNSUPPORTED = 6,     // the daemon does not serve leaderboards
   SCORES_MALFORMED = 7,       // the daemon refused the request's arguments
   SCORES_LOST = 8,            // the connection ended before the answer
};

// A script request, as the API thread queued it.
struct TopScoresAsk
{
   uint64_t epoch = 0;
   Wire::TopScoresRequest request;
};

// One result for the script, from the daemon or decided here.
struct TopScoresPost
{
   uint64_t epoch = 0;
   uint32_t requestId = 0;
   int result = SCORES_NONE;
   uint32_t ageS = 0;
   std::vector<Wire::TopScoreEntry> entries;
};

struct PlayerView
{
   std::string playerId;       // empty while the slot is unclaimed
   std::string preferredName;
   std::string name;
   std::string initials;
   std::string claimDeeplink;
   std::string picture;        // the plugin's own copy, UTF-8, or empty

   bool Claimed() const { return !playerId.empty(); }
   const std::string& DisplayName() const { return preferredName.empty() ? name : preferredName; }
   bool operator==(const PlayerView&) const = default;
};

struct PlayersPost
{
   uint64_t epoch = 0;
   std::array<PlayerView, Wire::MAX_PLAYERS> slots;
};

// Between the worker and the API thread. Every method is safe from either thread.
class ReadBackChannel final
{
public:
   // Requests waiting for the worker, and results waiting for the API thread; past it the oldest go.
   static constexpr size_t MAX_QUEUED = 16;

   // API thread. The request's id, or 0 when no daemon can answer it now or too many wait.
   uint32_t Request(uint64_t epoch, Wire::TopScoresRequest request);
   void TakeRequests(std::deque<TopScoresAsk>& out);

   // Worker: a session serves leaderboards and has a table declared.
   void SetLeaderboards(bool available) { m_leaderboards = available; }
   bool Leaderboards() const { return m_leaderboards; }

   void PostResult(TopScoresPost post);
   void TakeResults(std::deque<TopScoresPost>& out);

   void PostPlayers(const PlayersPost& post) { m_players.Post(post); }
   std::unique_ptr<PlayersPost> TakePlayers() { return m_players.Take(); }

private:
   std::atomic<bool> m_leaderboards { false };
   std::mutex m_mutex;
   uint32_t m_nextId = 1;      // kept within a script's positive Long
   std::deque<TopScoresAsk> m_requests;
   std::deque<TopScoresPost> m_results;
   Latch<PlayersPost> m_players;
};

// Where the worker keeps its picture copies, per user; empty when the platform names none.
std::filesystem::path DefaultPictureDirectory();

// The worker's slot table and the picture files it owns. Worker thread only.
class PlayerTable final
{
public:
   // The contract's cap: a bigger file is not the daemon's and is not copied.
   static constexpr uintmax_t MAX_PICTURE_BYTES = 2u << 20;

   enum class Picture { Ignored, Unchanged, Stored, Removed, Failed };

   PlayerTable() = default;
   ~PlayerTable() { Clear(); }
   PlayerTable(const PlayerTable&) = delete;
   PlayerTable& operator=(const PlayerTable&) = delete;

   // tag makes the copies' names unique to this plugin load.
   void SetDirectory(std::filesystem::path dir, uint64_t tag);

   // The whole table; a slot whose claim ends or changes loses its picture. True when anything changed.
   bool Apply(const Wire::Players& players);
   // Copies the file before returning, so the daemon may delete its own once this is acknowledged.
   // error is set on Failed, and never names a path.
   Picture Apply(const Wire::PlayerPicture& picture, std::string& error);
   // Empties every slot and deletes every copy. True when anything was held.
   bool Clear();

   void Fill(PlayersPost& out) const;

private:
   struct Slot
   {
      PlayerView view;
      std::string source;               // the daemon's path the copy was taken from
      std::filesystem::path copy;
   };
   static void DropPicture(Slot& slot);

   std::array<Slot, Wire::MAX_PLAYERS> m_slots;
   std::filesystem::path m_dir;
   uint64_t m_tag = 0;
   uint64_t m_copies = 0;
};

// The leaderboard results the script can read, by request id. API thread only.
class TopScoresStore final
{
public:
   using Clock = std::chrono::steady_clock;

   // Results kept; a new one evicts the oldest.
   static constexpr size_t CAPACITY = 8;

   struct Entry
   {
      TopScoresPost post;
      Clock::time_point received { };
   };

   void Add(TopScoresPost post, Clock::time_point now);
   const Entry* Find(uint32_t requestId) const;
   void Release(uint32_t requestId);
   void Clear() { m_entries.clear(); }

private:
   std::deque<Entry> m_entries;
};

// What Scorbit.Table reads. API thread only.
class ReadBackState final
{
public:
   using Clock = std::chrono::steady_clock;

   // A table switch: the players, pictures and results of the last one are gone.
   void NewEpoch(uint64_t epoch);
   uint64_t Epoch() const { return m_epoch; }
   // Takes what the worker posted for this epoch, dropping the rest.
   void Drain(ReadBackChannel& channel, Clock::time_point now);

   // Indexes as a script passes them: anything not a whole number in range reads as nothing.
   int Result(double requestId) const;
   int Age(double requestId, Clock::time_point now) const; // the daemon's age plus the time held here
   int Count(double requestId) const;
   const Wire::TopScoreEntry* Score(double requestId, double index) const;
   void Release(double requestId);
   const PlayerView* Slot(double slot) const; // 1 to 9

   TopScoresStore& Scores() { return m_scores; }

private:
   const TopScoresStore::Entry* Find(double requestId) const;

   uint64_t m_epoch = 0;
   TopScoresStore m_scores;
   PlayersPost m_players;
};

// v as a whole number from lo to hi, for an index from a script.
bool ScriptIndex(double v, double lo, double hi, uint32_t& out);

}
