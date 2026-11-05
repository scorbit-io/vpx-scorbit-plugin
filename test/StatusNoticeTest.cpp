// license:GPLv3+

// The Status latch and the transition-only notifier: what the API thread posts,
// frame by frame, for a given run of session states.

#include "StatusNotice.h"

#include "Check.h"

#include <chrono>
#include <memory>
#include <string>

using namespace Scorbit;
using Clock = StatusNotifier::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace
{

const std::string NO_SESSION = "[!] Scorbit: no data, no connection to the Scorbit daemon";

std::unique_ptr<StatusRecord> InSession(uint8_t session, uint8_t system, const std::string& text = "")
{
   auto r = std::make_unique<StatusRecord>();
   r->inSession = true;
   r->haveStatus = true;
   r->status = { session, system, text };
   return r;
}

std::unique_ptr<StatusRecord> Lost(Clock::time_point since, const std::string& reason = "")
{
   auto r = std::make_unique<StatusRecord>();
   r->since = since;
   r->reason = reason;
   return r;
}

// StatusNotices = 2, the behaviour before docs/status-notices.md: the posted text, or empty.
std::string Today(StatusNotifier& n, std::unique_ptr<StatusRecord> taken, bool show, Clock::time_point now)
{
   const NoticeAction a = n.Frame(std::move(taken), show, NOTICES_ALWAYS, false, now);
   return a.kind == NoticeAction::Kind::Post ? a.text : std::string();
}

// StatusNotices = 1, the default.
NoticeAction Quiet(StatusNotifier& n, std::unique_ptr<StatusRecord> taken, Clock::time_point now, bool authored = false)
{
   return n.Frame(std::move(taken), true, NOTICES_OUTSIDE_GAME, authored, now);
}

const NoticeAction NONE { };
NoticeAction Post(const std::string& text) { return { NoticeAction::Kind::Post, text }; }
NoticeAction Expire(const std::string& text) { return { NoticeAction::Kind::Expire, text }; }

// A record as the worker posts it for a ROM table: the Status plus what it remembers.
std::unique_ptr<StatusRecord> Rom(uint8_t session, uint8_t system, const std::string& text, StatusHistory history)
{
   auto r = InSession(session, system, text);
   r->history = std::move(history);
   return r;
}

StatusHistory InGame(uint32_t games = 1) { StatusHistory h; h.romGame = true; h.romGames = games; return h; }
StatusHistory Over(uint32_t games = 1) { StatusHistory h; h.romGames = games; return h; }
StatusHistory Warned(StatusHistory h, uint32_t warnings, const std::string& last) { h.warnings = warnings; h.lastWarning = last; return h; }

const std::string NO_GAME_OK = "[OK] Scorbit: no game, ok";

void TestLatchKeepsOnlyTheLatest()
{
   StatusLatch latch;
   CHECK(latch.Take() == nullptr);
   StatusRecord a, b;
   a.reason = "a";
   b.reason = "b";
   latch.Post(a);
   latch.Post(b);
   const std::unique_ptr<StatusRecord> taken = latch.Take();
   CHECK(taken != nullptr && taken->reason == "b");
   CHECK(latch.Take() == nullptr);
   latch.Post(a); // left in the slot; the destructor frees it
}

void TestNoSessionNoticeAfterFiveSeconds()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Today(n, nullptr, true, t0 + milliseconds(4999)).empty());
   CHECK(Today(n, nullptr, true, t0 + seconds(5)) == NO_SESSION);
   CHECK_MSG(Today(n, nullptr, true, t0 + seconds(6)).empty(), "posted once, not on a timer");
}

void TestRefusalReasonIsTheSupportText()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Today(n, Lost(t0, "busy: another VPX is connected"), true, t0 + seconds(5))
      == NO_SESSION + " - busy: another VPX is connected");
}

void TestTransitionsOnly()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   auto up = std::make_unique<StatusRecord>();
   up->inSession = true;
   CHECK_MSG(Today(n, std::move(up), true, t0 + seconds(10)).empty(), "nothing until the daemon's first Status");
   CHECK(Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0 + seconds(10)) == "[OK] Scorbit: tracking, ok");
   CHECK(Today(n, nullptr, true, t0 + seconds(11)).empty());
   CHECK_MSG(Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0 + seconds(12)).empty(),
      "the same state again is not a transition");
   CHECK(Today(n, InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_DEGRADED, "no map for this ROM"), true, t0 + seconds(13))
      == "[!] Scorbit: no data, degraded - no map for this ROM");
}

void TestQuickReconnectIsNotATransition()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!Today(n, InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, t0).empty());
   const Clock::time_point lost = t0 + seconds(30);
   CHECK(Today(n, Lost(lost), true, lost + seconds(2)).empty());
   auto up = std::make_unique<StatusRecord>();
   up->inSession = true;
   CHECK(Today(n, std::move(up), true, lost + seconds(3)).empty());
   CHECK(Today(n, InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, lost + seconds(3)).empty());
}

void TestLossIsCountedFromTheLoss()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0).empty());
   const Clock::time_point lost = t0 + seconds(60);
   CHECK(Today(n, Lost(lost), true, lost + milliseconds(4900)).empty());
   CHECK(Today(n, nullptr, true, lost + seconds(5)) == NO_SESSION);
}

void TestHiddenPostsNothingAndRepostsAfter()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0).empty());
   CHECK(Today(n, InSession(Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE), true, t0).empty());
   CHECK_MSG(Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0) == "[OK] Scorbit: tracking, ok",
      "the state after hidden posts, even though it matches the one before");
}

void TestShowStatusOffPostsNothing()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Today(n, nullptr, false, t0 + seconds(5)).empty());
   CHECK(Today(n, InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK), false, t0 + seconds(6)).empty());
   CHECK_MSG(Today(n, nullptr, true, t0 + seconds(7)) == "[!] Scorbit: no data, ok", "turned on, the current state posts");
}

void TestNewTableShowsTheCurrentStateOnce()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!Today(n, InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, t0).empty());
   n.NewTable();
   CHECK(Today(n, nullptr, true, t0 + seconds(1)) == "[OK] Scorbit: no game, ok");
   CHECK(Today(n, nullptr, true, t0 + seconds(2)).empty());
}

void TestDescribe()
{
   CHECK(StatusNotifier::Describe({ Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED, "" }) == "[!] Scorbit: no game, not paired");
   CHECK(StatusNotifier::Describe({ 9, 7, "" }) == "[?] Scorbit: unknown (9), unknown (7)");
   CHECK(StatusNotifier::Describe({ Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE, "" }) == "Scorbit: hidden");
}

// Working and not working differ in shape, not only in words.
void TestMarker()
{
   CHECK(StatusNotifier::Marker({ Wire::SESSION_NONE, Wire::SYSTEM_OK, "" }) == "[OK]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "" }) == "[OK]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "" }) == "[!]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_TRACKING, Wire::SYSTEM_UNPAIRED, "" }) == "[!]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_TRACKING, Wire::SYSTEM_OFFLINE, "" }) == "[!]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_NONE, Wire::SYSTEM_DEGRADED, "" }) == "[!]");
   CHECK(StatusNotifier::Marker({ 3, Wire::SYSTEM_OK, "" }) == "[?]");
   CHECK(StatusNotifier::Marker({ Wire::SESSION_TRACKING, 4, "" }) == "[?]");
}

// A [!] state that holds is posted again on the interval, and
// nothing else is. The API thread reuses its handle, so it stays one notification.
void TestWarningRefreshesOnTheInterval()
{
   const Clock::time_point t0 = Clock::now();
   const auto every = StatusNotifier::REFRESH_EVERY;
   StatusNotifier n(t0);
   CHECK(Today(n, nullptr, true, t0 + seconds(5)) == NO_SESSION);
   CHECK(Today(n, nullptr, true, t0 + seconds(5) + every - milliseconds(1)).empty());
   CHECK(Today(n, nullptr, true, t0 + seconds(5) + every) == NO_SESSION);
   CHECK_MSG(Today(n, nullptr, true, t0 + seconds(5) + every + seconds(1)).empty(), "timed from the last post");
   CHECK(Today(n, nullptr, true, t0 + seconds(5) + 2 * every) == NO_SESSION);

   const Clock::time_point t1 = t0 + seconds(1000);
   const std::string noData = "[!] Scorbit: no data, ok";
   CHECK(Today(n, InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK), true, t1) == noData);
   CHECK(Today(n, InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK), true, t1 + every - seconds(1)).empty());
   CHECK(Today(n, InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK), true, t1 + every) == noData);
}

void TestOnlyWarningsRefresh()
{
   const Clock::time_point t0 = Clock::now();
   const auto every = StatusNotifier::REFRESH_EVERY;
   StatusNotifier n(t0);
   CHECK(Today(n, InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0) == "[OK] Scorbit: tracking, ok");
   CHECK_MSG(Today(n, nullptr, true, t0 + 3 * every).empty(), "[OK] posts once");
   CHECK(Today(n, InSession(Wire::SESSION_TRACKING, 99), true, t0 + 3 * every) == "[?] Scorbit: tracking, unknown (99)");
   CHECK_MSG(Today(n, nullptr, true, t0 + 6 * every).empty(), "[?] posts once");
   CHECK(Today(n, InSession(Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE), true, t0 + 6 * every).empty());
   CHECK_MSG(Today(n, nullptr, true, t0 + 9 * every).empty(), "hidden never posts");
}

// --- StatusNotices = 1: nothing over the playfield while a game is in progress -------------

void TestRomGameIsQuiet()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(2)) == Expire(NO_GAME_OK),
      "a notice still on screen is taken down when the game starts");
   const std::string offline = "[!] Scorbit: tracking, offline - API unreachable";
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OFFLINE, "API unreachable", Warned(InGame(), 1, offline)), t0 + seconds(10)) == NONE,
      "a change mid-game is not posted");
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(80)) == NONE, "and not repeated");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Warned(Over(), 1, offline)), t0 + seconds(100))
      == Post("[!] Scorbit, during the game: tracking, offline - API unreachable"), "at the end, the [!] that came up meanwhile");
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(101)) == NONE, "the current state counts as shown; no second notice");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED, "", Warned(Over(), 2, "x")), t0 + seconds(102))
      == Post("[!] Scorbit: no game, not paired"), "outside a game, a change posts as before");
}

void TestEndOfGameCurrentWarning()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(6)) == NONE,
      "a notice older than its lifetime is not taken down");
   const std::string ended = "[!] Scorbit: no game, offline - x";
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OFFLINE, "x", Warned(Over(), 1, ended)), t0 + seconds(60)) == Post(ended));
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(60) + StatusNotifier::REFRESH_EVERY) == Post(ended), "and repeats outside a game");
}

void TestNoWarningNoEndNotice()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(10)) == NONE);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over()), t0 + seconds(60)) == NONE);
   CHECK(Quiet(n, nullptr, t0 + seconds(61)) == NONE);
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame(2)), t0 + seconds(62)) == NONE,
      "a game right after one that ended quietly has nothing to take down (seen live: a spurious take-down)");
}

void TestNoDataNeverStartsAGame()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   const std::string waiting = "[!] Scorbit: no data, ok - waiting for this table's first scores";
   CHECK(Quiet(n, Rom(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "waiting for this table's first scores", Warned({ }, 1, waiting)), t0) == Post(waiting));
   CHECK_MSG(Quiet(n, nullptr, t0 + StatusNotifier::REFRESH_EVERY) == Post(waiting), "outside a game, so it repeats");
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", Warned(InGame(), 1, waiting)), t0 + seconds(70)) == NONE);
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "", Warned(InGame(), 2, "[!] Scorbit: no data, ok")), t0 + seconds(80)) == NONE,
      "no data continues the game");
}

void TestNoGameDeferredAfterReconnect()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(10)) == NONE);
   StatusHistory deferred = InGame();
   deferred.romGameEndsAt = t0 + seconds(40);
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", deferred), t0 + seconds(35)) == NONE, "still in the game");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OFFLINE, "x", Warned(deferred, 1, "[!] Scorbit: no game, offline - x")), t0 + seconds(36)) == NONE,
      "a change while deferred is still in the game");
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(40)) == Post("[!] Scorbit: no game, offline - x"), "the game ends when the deferral is up");
}

void TestRomGameLostBound()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0) == NONE);
   auto lost = Lost(t0 + seconds(30));
   lost->history = InGame();
   CHECK(Quiet(n, std::move(lost), t0 + seconds(31)) == NONE);
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(36)) == NONE, "no connection is not posted mid-game");
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(30) + StatusNotifier::ROM_GAME_LOST_AFTER) == Post(NO_SESSION),
      "after the bound the game is over and the no-connection state posts");
}

void TestAuthoredGameFollowsTheScript()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0, true) == Post(NO_GAME_OK));
   n.GameStarted();
   CHECK(Quiet(n, nullptr, t0 + seconds(1), true) == Expire(NO_GAME_OK));
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over(3)), t0 + seconds(5), true) == NONE,
      "the daemon's session state plays no part");
   auto lost = Lost(t0 + seconds(10));
   lost->history = Over(3);
   CHECK(Quiet(n, std::move(lost), t0 + seconds(11), true) == NONE);
   CHECK(Quiet(n, nullptr, t0 + seconds(16), true) == NONE);
   n.GameEnded();
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(17), true) == Post(NO_SESSION), "the end-of-game notice carries the no-connection state");

   // An earlier [!] that an [OK] overtook before any frame saw it.
   StatusNotifier m(t0);
   CHECK(Quiet(m, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0, true) == Post(NO_GAME_OK));
   m.GameStarted();
   CHECK(Quiet(m, nullptr, t0 + seconds(10), true) == NONE);
   m.GameEnded();
   const std::string notRecorded = "[!] Scorbit: no data, ok - this game was not recorded";
   CHECK(Quiet(m, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Warned({ }, 1, notRecorded)), t0 + seconds(20), true)
      == Post("[!] Scorbit, during the game: no data, ok - this game was not recorded"));
}

void TestGameBetweenFrames()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   const std::string offline = "[!] Scorbit: tracking, offline - x";
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Warned(Over(), 1, offline)), t0 + seconds(30))
      == Post("[!] Scorbit, during the game: tracking, offline - x"), "a game that began and ended between frames still gets its notice");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over(2)), t0 + seconds(60)) == NONE, "one with nothing wrong gets none");
   // A table-load warning and the tracking that followed it arrived between two frames.
   StatusHistory loadWarned = Warned(Over(3), 2, "[!] Scorbit: no data, ok - waiting for this table's first scores");
   loadWarned.warningsAtGameStart = 2;
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", loadWarned), t0 + seconds(90)) == NONE,
      "a warning from before the game is not reported as during it");
}

void TestLostDuringGameIsReported()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0) == Post(NO_GAME_OK));
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(10)) == NONE);
   auto lost = Lost(t0 + seconds(30));
   lost->history = InGame();
   CHECK(Quiet(n, std::move(lost), t0 + seconds(31)) == NONE);
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(40)) == NONE, "past the grace, still quiet in the game");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(50)) == NONE, "reconnected within the bound");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over()), t0 + seconds(60))
      == Post("[!] Scorbit, during the game: no data, no connection to the Scorbit daemon"), "the outage is reported at the end");
}

void TestAuthoredEndSettles()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0, true) == Post(NO_GAME_OK));
   n.GameStarted();
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0 + seconds(10), true) == NONE);
   n.GameEnded();
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(30), true) == NONE, "a clean game ends with no notice");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over()), t0 + seconds(31), true) == NONE,
      "the daemon's own game end, arriving after the script's, is not news");
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED, "", Warned(Over(), 1, "x")), t0 + seconds(32), true)
      == Post("[!] Scorbit: no game, not paired"), "a [!] after it is");
   // The daemon's "not recorded" after a held authored game posts as a transition.
   StatusNotifier m(t0);
   CHECK(Quiet(m, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0, true) == Post(NO_GAME_OK));
   m.GameStarted();
   m.GameEnded();
   CHECK(Quiet(m, nullptr, t0 + seconds(10), true) == NONE);
   const std::string notRecorded = "[!] Scorbit: no data, ok - this game was not recorded";
   CHECK(Quiet(m, Rom(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK, "this game was not recorded", Warned({ }, 1, notRecorded)), t0 + seconds(11), true)
      == Post(notRecorded));
}

void TestEndSummaryCanBeTakenDown()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", { }), t0, true) == Post(NO_GAME_OK));
   n.GameStarted();
   const std::string offline = "[!] Scorbit: tracking, offline - x";
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OFFLINE, "x", Warned(InGame(), 1, offline)), t0 + seconds(10), true) == NONE);
   auto lost = Lost(t0 + seconds(20));
   lost->history = Warned(InGame(), 1, offline);
   CHECK(Quiet(n, std::move(lost), t0 + seconds(21), true) == NONE);
   n.GameEnded();
   const std::string summary = "[!] Scorbit, during the game: tracking, offline - x";
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(22), true) == Post(summary), "ended inside the grace: the summary posts with nothing current");
   n.GameStarted();
   CHECK_MSG(Quiet(n, nullptr, t0 + seconds(23), true) == Expire(summary), "and the next game can take it down");
}

void TestSettings()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier never(t0);
   CHECK(never.Frame(Rom(Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED, "", { }), true, NOTICES_NEVER, false, t0) == NONE);
   CHECK(never.Frame(nullptr, true, NOTICES_NEVER, false, t0 + seconds(5)) == NONE);
   CHECK_MSG(never.Frame(nullptr, true, NOTICES_OUTSIDE_GAME, false, t0 + seconds(6)) == Post("[!] Scorbit: no game, not paired"),
      "turned on, the current state posts");

   StatusNotifier always(t0);
   CHECK(Today(always, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), true, t0) == "[OK] Scorbit: tracking, ok");
   CHECK_MSG(Today(always, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OFFLINE, "x", Warned(InGame(), 1, "y")), true, t0 + seconds(1))
      == "[!] Scorbit: tracking, offline - x", "always: a change mid-game posts");
}

void TestTableEndedClearsTheGame()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(Quiet(n, Rom(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, "", InGame()), t0) == NONE);
   n.TableEnded();
   n.NewTable();
   CHECK_MSG(Quiet(n, Rom(Wire::SESSION_NONE, Wire::SYSTEM_OK, "", Over()), t0 + seconds(30)) == Post(NO_GAME_OK),
      "the next table shows its state once, with no end-of-game notice");
}

void TestDescribeEarlier()
{
   CHECK(StatusNotifier::DescribeEarlier("[!] Scorbit: no data, ok - x") == "[!] Scorbit, during the game: no data, ok - x");
   CHECK(StatusNotifier::DescribeEarlier(NO_SESSION) == "[!] Scorbit, during the game: no data, no connection to the Scorbit daemon");
}

}

int main()
{
   TestLatchKeepsOnlyTheLatest();
   TestNoSessionNoticeAfterFiveSeconds();
   TestRefusalReasonIsTheSupportText();
   TestTransitionsOnly();
   TestQuickReconnectIsNotATransition();
   TestLossIsCountedFromTheLoss();
   TestHiddenPostsNothingAndRepostsAfter();
   TestShowStatusOffPostsNothing();
   TestNewTableShowsTheCurrentStateOnce();
   TestDescribe();
   TestMarker();
   TestWarningRefreshesOnTheInterval();
   TestOnlyWarningsRefresh();
   TestRomGameIsQuiet();
   TestEndOfGameCurrentWarning();
   TestNoWarningNoEndNotice();
   TestNoDataNeverStartsAGame();
   TestNoGameDeferredAfterReconnect();
   TestRomGameLostBound();
   TestAuthoredGameFollowsTheScript();
   TestGameBetweenFrames();
   TestLostDuringGameIsReported();
   TestAuthoredEndSettles();
   TestEndSummaryCanBeTakenDown();
   TestSettings();
   TestTableEndedClearsTheGame();
   TestDescribeEarlier();
   return ScorbitTest::Summary("status_notice_test");
}
