// license:GPLv3+

// The status light's mapping: each state's dots, ring and glyph. Colours and cadences are written
// out as numbers rather than taken from the header, so a drifted constant fails here.

#include "StatusLight.h"

#include "Check.h"

#include <chrono>

using namespace Scorbit;
using Clock = StatusNotifier::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace
{

const Clock::time_point T0 = Clock::time_point { } + seconds(1000);

StatusRecord InSession(uint8_t session, uint8_t system)
{
   StatusRecord r;
   r.inSession = true;
   r.haveStatus = true;
   r.status.sessionState = session;
   r.status.systemState = system;
   return r;
}

StatusRecord WithAuth(uint8_t session, uint8_t system, uint8_t auth)
{
   StatusRecord r = InSession(session, system);
   r.status.hasAuthStatus = true;
   r.status.authStatus = auth;
   return r;
}

bool Is(const LightDot& dot, LightRgb rgb, uint16_t onMs, uint16_t offMs)
{
   return dot.lit && dot.rgb == rgb && dot.onMs == onMs && dot.offMs == offMs;
}

bool SystemIs(const LightView& v, LightRgb rgb)
{
   return Is(v.system, rgb, 1200, 100);
}

bool SessionIs(const LightView& v, LightRgb rgb)
{
   return Is(v.session, rgb, 1189, 123);
}

LightView For(const StatusRecord& r)
{
   return LightFor(r, T0, { });
}

void TestSessionDotMatchesTheDevice()
{
   CHECK_MSG(SessionIs(For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK)), { 0, 0, 255 }), "no game is blue");
   CHECK_MSG(SessionIs(For(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK)), { 0, 255, 0 }), "tracking is green");
   CHECK_MSG(SessionIs(For(InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK)), { 255, 191, 0 }), "no data is amber");
}

void TestSystemDotMatchesTheDevice()
{
   CHECK_MSG(SystemIs(For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK)), { 0, 255, 0 }), "ok is green");
   CHECK_MSG(SystemIs(For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OFFLINE)), { 255, 165, 0 }), "offline is orange");
   CHECK_MSG(SystemIs(For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED)), { 255, 0, 0 }), "unpaired is red");
   CHECK_MSG(SystemIs(For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_DEGRADED)), { 255, 0, 255 }), "degraded is purple");
}

void TestRingAndGlyph()
{
   const LightView idle = For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OK));
   CHECK(idle.visible && idle.ring == LightRing::Rainbow && idle.glyph == LightGlyph::None);

   const LightView game = For(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK));
   CHECK_MSG(game.ring == LightRing::Dark && game.glyph == LightGlyph::None, "the ring is dark while a game is scored");

   const LightView noData = For(InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OK));
   CHECK(noData.ring == LightRing::Dark && noData.glyph == LightGlyph::Problem);

   const LightView offline = For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_OFFLINE));
   CHECK_MSG(offline.ring == LightRing::Dark && offline.glyph == LightGlyph::Problem, "offline is no rainbow");

   const LightView unpaired = For(InSession(Wire::SESSION_NONE, Wire::SYSTEM_UNPAIRED));
   CHECK_MSG((unpaired.ring == LightRing::Solid && unpaired.ringRgb == LightRgb { 255, 0, 0 } && unpaired.glyph == LightGlyph::Problem),
      "not provisioned is a red ring");
}

void TestUnauthorizedIsRedLikeTheDevice()
{
   for (uint8_t auth : { Wire::AUTH_NOT_AUTHENTICATED, Wire::AUTH_FAILED })
   {
      const LightView v = For(WithAuth(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, auth));
      CHECK_MSG(SessionIs(v, { 255, 0, 0 }), "no credentials outrank the game state");
      CHECK((v.ring == LightRing::Solid && v.ringRgb == LightRgb { 255, 0, 0 } && v.glyph == LightGlyph::Problem));
   }
   // Unpaired by the authentication state alone, with the system state still ok.
   const LightView unpaired = For(WithAuth(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, Wire::AUTH_UNPAIRED));
   CHECK_MSG(SystemIs(unpaired, { 255, 0, 0 }) && SessionIs(unpaired, { 0, 255, 0 }), "the system dot says unpaired, the session stands");
   CHECK((unpaired.ring == LightRing::Solid && unpaired.ringRgb == LightRgb { 255, 0, 0 } && unpaired.glyph == LightGlyph::Problem));
   CHECK_MSG(SystemIs(For(WithAuth(Wire::SESSION_NONE, Wire::SYSTEM_OFFLINE, Wire::AUTH_UNPAIRED)), { 255, 165, 0 }),
      "a system dot already showing a problem keeps it");
   for (uint8_t auth : { Wire::AUTH_AUTHENTICATING, Wire::AUTH_CHECKING_PAIRING, Wire::AUTH_PAIRED })
      CHECK(SessionIs(For(WithAuth(Wire::SESSION_TRACKING, Wire::SYSTEM_OK, auth)), { 0, 255, 0 }));
   CHECK_MSG(SessionIs(For(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK)), { 0, 255, 0 }),
      "a daemon without the 1.2 tail keeps its session state");
}

void TestUnknownValues()
{
   const LightView system = For(InSession(Wire::SESSION_TRACKING, 99));
   CHECK(!system.system.lit && system.session.lit && system.glyph == LightGlyph::Unknown && system.ring == LightRing::Dark);
   const LightView session = For(InSession(99, Wire::SYSTEM_OK));
   CHECK(session.system.lit && !session.session.lit && session.glyph == LightGlyph::Unknown);
}

void TestHiddenShowsNothing()
{
   CHECK(!For(InSession(Wire::SESSION_HIDDEN, Wire::SYSTEM_OFFLINE)).visible);
}

void TestNoDaemonAfterTheGrace()
{
   const LightView shown = For(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK));
   StatusRecord lost;
   lost.since = T0;
   CHECK_MSG(LightFor(lost, T0 + milliseconds(4999), shown) == shown, "a loss inside the grace keeps the light");
   const LightView none = LightFor(lost, T0 + seconds(5), shown);
   CHECK_MSG(none.visible && !none.system.lit && !none.session.lit, "no daemon drives no dot");
   CHECK((none.ring == LightRing::Solid && none.ringRgb == LightRgb { 0, 0, 255 } && none.glyph == LightGlyph::Problem));

   StatusRecord waiting;
   waiting.inSession = true;
   CHECK_MSG(LightFor(waiting, T0, none) == none, "a session awaiting its first Status keeps the light");
}

// The caption the light shows on hover: the notifier's own description, or nothing while there is
// nothing to say.
void TestCaptionIsTheDescribedState()
{
   StatusRecord lost;
   lost.since = T0;
   CHECK(CaptionFor(lost, T0 + milliseconds(4999)).empty());
   CHECK(CaptionFor(lost, T0 + seconds(5)) == "[!] Scorbit: no data, no connection to the Scorbit daemon");
   lost.reason = "busy: another VPX is connected";
   CHECK(CaptionFor(lost, T0 + seconds(5)) == "[!] Scorbit: no data, no connection to the Scorbit daemon - busy: another VPX is connected");

   StatusRecord waiting;
   waiting.inSession = true;
   CHECK_MSG(CaptionFor(waiting, T0).empty(), "a session awaiting its first Status says nothing");
   CHECK(CaptionFor(InSession(Wire::SESSION_TRACKING, Wire::SYSTEM_OK), T0) == "[OK] Scorbit: tracking, ok");
   StatusRecord offline = InSession(Wire::SESSION_NO_DATA, Wire::SYSTEM_OFFLINE);
   offline.status.supportText = "Scorbit API unreachable";
   CHECK(CaptionFor(offline, T0) == "[!] Scorbit: no data, offline - Scorbit API unreachable");
   CHECK_MSG(CaptionFor(InSession(Wire::SESSION_HIDDEN, Wire::SYSTEM_OK), T0).empty(), "hidden shows no caption");
}

// The hover rule: a moving cursor over the light, and nothing after 3 s at rest.
void TestHoverNeedsAMovingCursor()
{
   HoverTracker h;
   CHECK_MSG(h.Tick(10, 10, true, false, T0), "the first position counts as a move");
   CHECK(h.Tick(10, 10, true, false, T0 + milliseconds(2999)));
   CHECK_MSG(!h.Tick(10, 10, true, false, T0 + seconds(3)), "at rest for 3 s: gone, as when VPX hid the cursor on a press");
   CHECK_MSG(!h.Tick(10, 10, true, false, T0 + seconds(30)), "and stays gone");
   CHECK_MSG(h.Tick(11, 10, true, false, T0 + seconds(31)), "fresh movement brings it back");
   CHECK_MSG(!h.Tick(12, 10, false, false, T0 + seconds(32)), "moving, but not over the light");
   CHECK_MSG(!h.Tick(13, 10, true, true, T0 + seconds(33)), "over the light, but the cursor is hidden");
   CHECK_MSG(h.Tick(13, 10, true, false, T0 + seconds(34)), "shown again once visible, within the window of that move");
}

}

int main()
{
   TestSessionDotMatchesTheDevice();
   TestSystemDotMatchesTheDevice();
   TestRingAndGlyph();
   TestUnauthorizedIsRedLikeTheDevice();
   TestUnknownValues();
   TestHiddenShowsNothing();
   TestNoDaemonAfterTheGrace();
   TestCaptionIsTheDescribedState();
   TestHoverNeedsAMovingCursor();
   return ScorbitTest::Summary("status_light_test");
}
