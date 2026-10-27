// license:GPLv3+

// The Status latch and the transition-only notifier (scorbitd SPEC-0002 REQ-6.3, 6.7,
// 6.11): what the API thread posts, frame by frame, for a given run of session states.

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
   CHECK(n.Frame(nullptr, true, t0 + milliseconds(4999)).empty());
   CHECK(n.Frame(nullptr, true, t0 + seconds(5)) == NO_SESSION);
   CHECK_MSG(n.Frame(nullptr, true, t0 + seconds(6)).empty(), "posted once, not on a timer");
}

void TestRefusalReasonIsTheSupportText()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(n.Frame(Lost(t0, "busy: another VPX is connected"), true, t0 + seconds(5))
      == NO_SESSION + " - busy: another VPX is connected");
}

void TestTransitionsOnly()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   auto up = std::make_unique<StatusRecord>();
   up->inSession = true;
   CHECK_MSG(n.Frame(std::move(up), true, t0 + seconds(10)).empty(), "nothing until the daemon's first Status");
   CHECK(n.Frame(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0 + seconds(10)) == "[OK] Scorbit: tracking, ok");
   CHECK(n.Frame(nullptr, true, t0 + seconds(11)).empty());
   CHECK_MSG(n.Frame(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0 + seconds(12)).empty(),
      "the same state again is not a transition");
   CHECK(n.Frame(InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_DEGRADED, "no map for this ROM"), true, t0 + seconds(13))
      == "[!] Scorbit: no data, degraded - no map for this ROM");
}

void TestQuickReconnectIsNotATransition()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!n.Frame(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, t0).empty());
   const Clock::time_point lost = t0 + seconds(30);
   CHECK(n.Frame(Lost(lost), true, lost + seconds(2)).empty());
   auto up = std::make_unique<StatusRecord>();
   up->inSession = true;
   CHECK(n.Frame(std::move(up), true, lost + seconds(3)).empty());
   CHECK(n.Frame(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, lost + seconds(3)).empty());
}

void TestLossIsCountedFromTheLoss()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!n.Frame(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0).empty());
   const Clock::time_point lost = t0 + seconds(60);
   CHECK(n.Frame(Lost(lost), true, lost + milliseconds(4900)).empty());
   CHECK(n.Frame(nullptr, true, lost + seconds(5)) == NO_SESSION);
}

void TestHiddenPostsNothingAndRepostsAfter()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!n.Frame(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0).empty());
   CHECK(n.Frame(InSession(Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE), true, t0).empty());
   CHECK_MSG(n.Frame(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), true, t0) == "[OK] Scorbit: tracking, ok",
      "the state after hidden posts, even though it matches the one before");
}

void TestShowStatusOffPostsNothing()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(n.Frame(nullptr, false, t0 + seconds(5)).empty());
   CHECK(n.Frame(InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK), false, t0 + seconds(6)).empty());
   CHECK_MSG(n.Frame(nullptr, true, t0 + seconds(7)) == "[!] Scorbit: no data, ok", "turned on, the current state posts");
}

void TestNewTableShowsTheCurrentStateOnce()
{
   const Clock::time_point t0 = Clock::now();
   StatusNotifier n(t0);
   CHECK(!n.Frame(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK), true, t0).empty());
   n.NewTable();
   CHECK(n.Frame(nullptr, true, t0 + seconds(1)) == "[OK] Scorbit: no game, ok");
   CHECK(n.Frame(nullptr, true, t0 + seconds(2)).empty());
}

void TestDescribe()
{
   CHECK(StatusNotifier::Describe({ Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED, "" }) == "[!] Scorbit: no game, not paired");
   CHECK(StatusNotifier::Describe({ 9, 7, "" }) == "[?] Scorbit: unknown (9), unknown (7)");
   CHECK(StatusNotifier::Describe({ Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE, "" }) == "Scorbit: hidden");
}

// REQ-6.5: working and not working differ in shape, not only in words.
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
   return ScorbitTest::Summary("status_notice_test");
}
