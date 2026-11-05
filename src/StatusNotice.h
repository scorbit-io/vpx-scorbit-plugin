// license:GPLv3+

#pragma once

// Status indication: what the socket worker knows of
// the daemon's Status and of the session, handed to the API thread, and turned there into
// a notification on transitions, repeated while a [!] state lasts, and kept off the
// playfield while a game is in progress (docs/status-notices.md). No
// plugin-API surface can carry the status indicator yet (SB-5996), so the notification is
// the interim primary surface.

#include "Latch.h"
#include "WireProtocol.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace Scorbit
{

// What the worker remembers for the table across sessions, from every Status it receives.
// The notifier sees only the latest record a frame finds in the latch, so a game that began
// and ended between two frames, or a [!] that an [OK] overtook, survives here as counts.
struct StatusHistory
{
   bool romGame = false;      // a ROM game is in progress: tracking, until no game or the table ends
   uint32_t romGames = 0;     // ROM games started
   // Set while a no game that arrived within 10 s of a reconnect is deferred: the game ends
   // then unless a tracking or no data arrives first.
   std::chrono::steady_clock::time_point romGameEndsAt { };
   uint32_t warnings = 0;     // [!] Statuses received
   std::string lastWarning;   // the last one, described
};

// The whole state the API thread shows, so the latest one record is enough.
struct StatusRecord
{
   bool inSession = false;    // between HelloAck and the end of that session
   bool haveStatus = false;   // a Status arrived in this session
   Wire::Status status;
   std::string reason;        // why there is no session, when the daemon said why
   std::chrono::steady_clock::time_point since { }; // when there stopped being a session
   StatusHistory history;
};

// Worker to API thread. An overwritten record lasted under a frame, which is shorter than
// anyone could read it.
using StatusLatch = Latch<StatusRecord>;

// [Plugin.Scorbit] StatusNotices.
enum StatusNotices : int
{
   NOTICES_NEVER = 0,         // the light only
   NOTICES_OUTSIDE_GAME = 1,  // and one at the end of a game that had a [!] state
   NOTICES_ALWAYS = 2,        // on every change, in a game or not
};

// What a frame does with VPX's notification: nothing, post this text, or take down the one
// posted (its text, so a handle VPX already dropped shows nothing new).
struct NoticeAction
{
   enum class Kind { None, Post, Expire };
   Kind kind = Kind::None;
   std::string text;

   bool operator==(const NoticeAction&) const = default;
};

// API thread. Decides, once per rendered frame, whether a notification is due.
class StatusNotifier final
{
public:
   using Clock = std::chrono::steady_clock;

   // Without a session, the amber notice waits this long after the session went.
   static constexpr std::chrono::seconds NO_SESSION_AFTER { 5 };
   // A [!] state still holding is posted again this long after it last was.
   static constexpr std::chrono::seconds REFRESH_EVERY { 60 };
   // How long VPX shows a notice; one younger than this is taken down when a game starts.
   static constexpr std::chrono::seconds NOTICE_LIFETIME { 5 };
   // A ROM game's end can only come from the daemon, so without one it is kept this long.
   static constexpr std::chrono::minutes ROM_GAME_LOST_AFTER { 2 };
   // A restarted daemon says no game until it has read the machine: one this soon after a
   // reconnect is deferred this long, and a tracking or no data meanwhile continues the game.
   static constexpr std::chrono::seconds NO_GAME_DEFERRED { 10 };

   // start counts as the moment there stopped being a session, for a plugin that never had one.
   explicit StatusNotifier(Clock::time_point start);

   // taken is what the latch held, if anything. authored: the table's script scores it, so
   // its game runs from GameStarted to GameEnded and the daemon's session state plays no part.
   NoticeAction Frame(std::unique_ptr<StatusRecord> taken, bool showStatus, int notices, bool authored, Clock::time_point now);

   // A new table starts: it shows the current state once, even if unchanged.
   void NewTable();
   // The table ended: nothing renders after it, so the game ends without a notice.
   void TableEnded();
   // The script's game_start and game_end, as the session source commits them.
   void GameStarted();
   void GameEnded();

   // "[OK] Scorbit: tracking, ok", plus " - <support text>" when there is one. The leading
   // marker is the shape distinction the status indication needs, so no state is told by colour alone.
   static std::string Describe(const Wire::Status& status);
   // [OK] working, [!] expected to work and not, [?] a value this build does not know.
   static std::string Marker(const Wire::Status& status);
   static std::string DescribeNoSession(const std::string& reason);
   // A [!] that came up during a game and is over by its end: "[!] Scorbit, during the game: ...".
   static std::string DescribeEarlier(const std::string& described);

private:
   // The text the current state calls for, or empty while there is nothing to say.
   std::string Want(Clock::time_point now) const;
   bool RomGameInProgress(Clock::time_point now) const;

   StatusRecord m_current;
   std::string m_posted; // last text posted; cleared while nothing may show, so the state reposts after
   Clock::time_point m_postedAt { };
   bool m_inGame = false;              // as of the last frame
   bool m_authoredGame = false;        // GameStarted, until GameEnded or the table ends
   uint32_t m_authoredGames = 0;       // GameStarted calls, counted like the worker's ROM games
   bool m_authoredLast = false;        // the kind the last frame saw, so a switch does not read as a start
   uint32_t m_seenStarts = 0;          // the start count as of the last frame
   uint32_t m_seenWarnings = 0;        // the warning count as of the last frame
   uint32_t m_warningsAtStart = 0;     // the warning count when the current game began
};

}
