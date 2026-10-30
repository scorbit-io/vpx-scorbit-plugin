// license:GPLv3+

#include "ScriptBinding.h"

#include "common.h"
#include "VpxSessionSource.h"

// The upstream header carries MSVC-only #pragma warning lines, and its PSC_CLASS_START
// leaves ScriptTypeNameDef::id to zero-initialisation; neither is ours to change.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "plugins/ScriptablePlugin.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Scorbit
{

namespace
{

constexpr const char* PROG_ID = "Scorbit.Table";

const MsgPluginAPI* s_msgApi = nullptr;
unsigned int s_getScriptApiId = 0;
ScriptablePluginAPI* s_scriptApi = nullptr;
VpxSessionSource* s_session = nullptr; // null once the plugin unloads, whatever objects remain

// Longest mode name passed on, in UTF-8 bytes; a longer one is refused rather than cut,
// which could split a character or merge two modes.
constexpr size_t MAX_MODE_BYTES = 128;
// Scores reach the plugin as Double; past 2^53 a Double no longer holds every integer.
constexpr double MAX_EXACT_SCORE = 9007199254740992.0;

// 1 is the game calls; 2 adds the read-backs. Raised as features land, never lowered.
constexpr int API_LEVEL = 2;

#ifndef SCORBIT_PLUGIN_VERSION
#define SCORBIT_PLUGIN_VERSION "0.0.0"
#endif

const std::string EMPTY;

// One per CreateObject; every instance speaks for the same table. Every call is logged
// and ignored when its arguments do not fit, never raised: a script error stops the table.
class ScorbitTable final
{
public:
   PSC_IMPLEMENT_REFCOUNT()

   void Declare(const char* machineId)
   {
      if (s_session != nullptr)
         s_session->DeclareMachine(machineId != nullptr ? machineId : "");
   }

   void SetGameStarted(double origin) { Small("SetGameStarted", Wire::GAME_START, origin); }
   void SetGameFinished()
   {
      Op(Of(Wire::GAME_END));
      Commit();
   }
   void SetCurrentBall(double ball) { Small("SetCurrentBall", Wire::GAME_BALL, ball); }
   void SetActivePlayer(double player) { Small("SetActivePlayer", Wire::GAME_ACTIVE_PLAYER, player); }

   void SetScore(double player, double score, double feature)
   {
      if (s_session == nullptr)
         return;
      // A fraction is refused rather than truncated, so 12.9 never silently scores 12.
      if (!std::isfinite(score) || std::fabs(score) > MAX_EXACT_SCORE || score != std::trunc(score))
      {
         s_session->ScriptCallIgnored("Scorbit.SetScore ignored, the score is not an integer within 2^53");
         return;
      }
      Wire::GameOp op = Of(Wire::GAME_SCORE);
      if (!SmallValue("SetScore", player, op.value) || !InRange("SetScore", feature, -2147483648.0, 2147483647.0))
         return;
      op.score = static_cast<int64_t>(score);
      op.feature = static_cast<int32_t>(feature);
      Op(op);
   }

   void AddMode(const char* mode) { Mode("AddMode", Wire::GAME_ADD_MODE, mode); }
   void AddModeExpiring(double seconds, const char* mode)
   {
      if (s_session == nullptr || !InRange("AddModeExpiring", seconds, 0.0, 4294967295.0))
         return;
      Mode("AddModeExpiring", Wire::GAME_ADD_MODE_EXPIRING, mode, static_cast<uint32_t>(seconds));
   }
   void SetModeCompleted(const char* mode) { Mode("SetModeCompleted", Wire::GAME_MODE_COMPLETED, mode); }
   void RemoveMode(const char* mode) { Mode("RemoveMode", Wire::GAME_REMOVE_MODE, mode); }
   void ClearModes() { Op(Of(Wire::GAME_CLEAR_MODES)); }

   void Commit()
   {
      if (s_session != nullptr)
         s_session->ScriptCommit();
   }

   // --- Read-backs (ApiLevel 2). Read-only; anything out of range reads as empty or 0. ---

   std::string GetVersion() const { return SCORBIT_PLUGIN_VERSION; }
   int GetApiLevel() const { return API_LEVEL; }

   // -1 before the session's first Status.
   int GetSystemState() const
   {
      const StatusRecord* r = Status();
      return r != nullptr ? r->status.systemState : -1;
   }
   int GetSessionState() const
   {
      const StatusRecord* r = Status();
      return r != nullptr ? r->status.sessionState : -1;
   }
   // -1 until a Status carries it, which a daemon before protocol 1.2 never does.
   int GetAuthStatus() const
   {
      const StatusRecord* r = Status();
      return r != nullptr && r->status.hasAuthStatus ? r->status.authStatus : -1;
   }
   std::string GetStatusText() const
   {
      const StatusRecord* r = Status();
      return r != nullptr ? r->status.supportText : std::string();
   }
   std::string GetMachineUuid() const { return s_session != nullptr ? s_session->DeclaredMachineUuid() : std::string(); }

   int RequestTopScores(double scope, double period, double vpinFilter, double limit, const char* since)
   {
      if (s_session == nullptr)
         return 0;
      Wire::TopScoresRequest req;
      uint32_t v[4] = { };
      req.since = since != nullptr ? since : "";
      if (!ScriptIndex(scope, Wire::SCOPE_MACHINE, Wire::SCOPE_GAME, v[0]) || !ScriptIndex(period, Wire::PERIOD_ALL_TIME, Wire::PERIOD_DAYS_365, v[1])
         || !ScriptIndex(vpinFilter, Wire::VPIN_ANY, Wire::VPIN_REAL_ONLY, v[2]) || !ScriptIndex(limit, 1, Wire::MAX_TOP_SCORES, v[3])
         || req.since.size() > Wire::MAX_SINCE_BYTES)
      {
         s_session->ScriptCallIgnored("Scorbit.RequestTopScores ignored, scope 0-2, period 0-5, filter 0-2, limit 1-20 and since up to 32 bytes");
         return 0;
      }
      req.scope = static_cast<uint8_t>(v[0]);
      req.period = static_cast<uint8_t>(v[1]);
      req.vpinFilter = static_cast<uint8_t>(v[2]);
      req.limit = static_cast<uint8_t>(v[3]);
      return static_cast<int>(s_session->ScriptRequestTopScores(req));
   }

   int TopScoresResult(double id) { return ReadBack() != nullptr ? ReadBack()->Result(id) : SCORES_NONE; }
   int TopScoresAge(double id) { return ReadBack() != nullptr ? ReadBack()->Age(id, std::chrono::steady_clock::now()) : 0; }
   int TopScoresCount(double id) { return ReadBack() != nullptr ? ReadBack()->Count(id) : 0; }
   int TopScoresRank(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? e->rank : 0; }
   double TopScoresScore(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? static_cast<double>(e->highScore) : 0; }
   std::string TopScoresName(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? e->displayName : EMPTY; }
   std::string TopScoresInitials(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? e->initials : EMPTY; }
   int TopScoresFlags(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? e->flags : 0; }
   std::string TopScoresCreated(double id, double i) { const Wire::TopScoreEntry* e = Score(id, i); return e != nullptr ? e->created : EMPTY; }
   void ReleaseTopScores(double id)
   {
      if (ReadBack() != nullptr)
         ReadBack()->Release(id);
   }

   bool PlayerClaimed(double slot) { const PlayerView* p = Player(slot); return p != nullptr && p->Claimed(); }
   std::string PlayerId(double slot) { const PlayerView* p = Player(slot); return p != nullptr ? p->playerId : EMPTY; }
   std::string PlayerName(double slot) { const PlayerView* p = Player(slot); return p != nullptr ? p->DisplayName() : EMPTY; }
   std::string PlayerInitials(double slot) { const PlayerView* p = Player(slot); return p != nullptr ? p->initials : EMPTY; }
   std::string PlayerClaimLink(double slot) { const PlayerView* p = Player(slot); return p != nullptr ? p->claimDeeplink : EMPTY; }
   std::string PlayerPicture(double slot) { const PlayerView* p = Player(slot); return p != nullptr ? p->picture : EMPTY; }

private:
   static const StatusRecord* Status()
   {
      if (s_session == nullptr)
         return nullptr;
      const StatusRecord& r = s_session->ScriptStatus();
      return r.inSession && r.haveStatus ? &r : nullptr;
   }

   static ReadBackState* ReadBack() { return s_session != nullptr ? &s_session->ScriptReadBack() : nullptr; }
   static const Wire::TopScoreEntry* Score(double id, double i) { return ReadBack() != nullptr ? ReadBack()->Score(id, i) : nullptr; }
   static const PlayerView* Player(double slot) { return ReadBack() != nullptr ? ReadBack()->Slot(slot) : nullptr; }

   static Wire::GameOp Of(uint8_t kind)
   {
      Wire::GameOp op;
      op.kind = kind;
      return op;
   }

   static void Op(const Wire::GameOp& op)
   {
      if (s_session != nullptr)
         s_session->ScriptGameOp(op);
   }

   // Numbers arrive as double: VPX converts an int parameter before this code runs and raises a
   // script error on overflow, so the range is checked here, where it can be logged instead.
   static bool InRange(const char* verb, double v, double lo, double hi)
   {
      if (std::isfinite(v) && v == std::trunc(v) && v >= lo && v <= hi)
         return true;
      s_session->ScriptCallIgnored("Scorbit."s + verb + " ignored, an argument is not a whole number from "
         + std::to_string(static_cast<long long>(lo)) + " to " + std::to_string(static_cast<long long>(hi)));
      return false;
   }

   // Range checks are the SDK's (contract §3); this only refuses what a u8 cannot carry.
   static bool SmallValue(const char* verb, double v, uint8_t& out)
   {
      if (!InRange(verb, v, 0.0, 255.0))
         return false;
      out = static_cast<uint8_t>(v);
      return true;
   }

   static void Small(const char* verb, uint8_t kind, double v)
   {
      Wire::GameOp op = Of(kind);
      if (s_session != nullptr && SmallValue(verb, v, op.value))
         Op(op);
   }

   // The host hands strings over as UTF-8.
   static void Mode(const char* verb, uint8_t kind, const char* mode, uint32_t seconds = 0)
   {
      if (s_session == nullptr)
         return;
      Wire::GameOp op = Of(kind);
      op.seconds = seconds;
      op.mode = mode != nullptr ? mode : "";
      if (op.mode.size() > MAX_MODE_BYTES)
      {
         s_session->ScriptCallIgnored("Scorbit."s + verb + " ignored, the mode is over "
            + std::to_string(MAX_MODE_BYTES) + " bytes");
         return;
      }
      Op(op);
   }
};

PSC_CLASS_START(Scorbit_Table, ScorbitTable)
   // The string members are by hand rather than PSC_FUNCTIONn, whose string conversion
   // would not survive a null.
   members.push_back({ { "Declare", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->Declare(pArgs[0].vString.string); } });
   PSC_FUNCTION1(void, SetGameStarted, double)
   PSC_FUNCTION0(void, SetGameFinished)
   PSC_FUNCTION1(void, SetCurrentBall, double)
   PSC_FUNCTION1(void, SetActivePlayer, double)
   // Two members of one name: VPX picks the overload by argument count, so feature is optional.
   members.push_back({ { "SetScore", 0 }, { "void", 0 }, 2, { { "double", 0 }, { "double", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->SetScore(pArgs[0].vDouble, pArgs[1].vDouble, 0); } });
   PSC_FUNCTION3(void, SetScore, double, double, double)
   members.push_back({ { "AddMode", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->AddMode(pArgs[0].vString.string); } });
   members.push_back({ { "AddModeExpiring", 0 }, { "void", 0 }, 2, { { "double", 0 }, { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->AddModeExpiring(pArgs[0].vDouble, pArgs[1].vString.string); } });
   members.push_back({ { "SetModeCompleted", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->SetModeCompleted(pArgs[0].vString.string); } });
   members.push_back({ { "RemoveMode", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->RemoveMode(pArgs[0].vString.string); } });
   PSC_FUNCTION0(void, ClearModes)
   PSC_FUNCTION0(void, Commit)
   // ApiLevel 2: read-backs. Indexed members are functions, as the header has no two-index property.
   PSC_PROP_R(string, Version)
   PSC_PROP_R(int, ApiLevel)
   PSC_PROP_R(int, SystemState)
   PSC_PROP_R(int, SessionState)
   PSC_PROP_R(int, AuthStatus)
   PSC_PROP_R(string, StatusText)
   PSC_PROP_R(string, MachineUuid)
   members.push_back({ { "RequestTopScores", 0 }, { "int", 0 }, 5, { { "double", 0 }, { "double", 0 }, { "double", 0 }, { "double", 0 }, { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant* pRet) { const int id = static_cast<ScorbitTable*>(me)->RequestTopScores(
         pArgs[0].vDouble, pArgs[1].vDouble, pArgs[2].vDouble, pArgs[3].vDouble, pArgs[4].vString.string); if (pRet != nullptr) pRet->vInt = id; } });
   PSC_FUNCTION1(int, TopScoresResult, double)
   PSC_FUNCTION1(int, TopScoresAge, double)
   PSC_FUNCTION1(int, TopScoresCount, double)
   PSC_FUNCTION2(int, TopScoresRank, double, double)
   PSC_FUNCTION2(double, TopScoresScore, double, double)
   PSC_FUNCTION2(string, TopScoresName, double, double)
   PSC_FUNCTION2(string, TopScoresInitials, double, double)
   PSC_FUNCTION2(int, TopScoresFlags, double, double)
   PSC_FUNCTION2(string, TopScoresCreated, double, double)
   PSC_FUNCTION1(void, ReleaseTopScores, double)
   PSC_FUNCTION1(bool, PlayerClaimed, double)
   PSC_FUNCTION1(string, PlayerId, double)
   PSC_FUNCTION1(string, PlayerName, double)
   PSC_FUNCTION1(string, PlayerInitials, double)
   PSC_FUNCTION1(string, PlayerClaimLink, double)
   PSC_FUNCTION1(string, PlayerPicture, double)
PSC_CLASS_END()

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

}

bool RegisterScriptBinding(const MsgPluginAPI* msgApi, uint32_t endpointId, VpxSessionSource* session)
{
   s_msgApi = msgApi;
   s_session = session;
   s_getScriptApiId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, s_getScriptApiId, &s_scriptApi);
   if (s_scriptApi == nullptr)
   {
      LOGW("script: VPX offers no scriptable API, so "s + PROG_ID + " is unavailable to table scripts");
      return false;
   }
   RegisterScorbit_Table([](ScriptClassDef* scd) { s_scriptApi->RegisterScriptClass(scd); });
   s_scriptApi->SubmitTypeLibrary(endpointId);
   Scorbit_Table_SCD->CreateObject = []() -> void* { return new ScorbitTable(); };
   s_scriptApi->SetCOMObjectOverride(PROG_ID, Scorbit_Table_SCD);
   return true;
}

void UnregisterScriptBinding(bool atQuit)
{
   s_session = nullptr;
   if (atQuit)
      return;
   if (s_scriptApi != nullptr)
   {
      s_scriptApi->SetCOMObjectOverride(PROG_ID, nullptr);
      UnregisterScorbit_Table([](ScriptClassDef* scd) { s_scriptApi->UnregisterScriptClass(scd); });
      s_scriptApi = nullptr;
   }
   if (s_msgApi != nullptr)
   {
      s_msgApi->ReleaseMsgID(s_getScriptApiId);
      s_msgApi = nullptr;
   }
}

}
