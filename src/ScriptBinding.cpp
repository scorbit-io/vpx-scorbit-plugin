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

   void SetGameStarted(int origin) { Small("SetGameStarted", Wire::GAME_START, origin); }
   void SetGameFinished()
   {
      Op(Of(Wire::GAME_END));
      Commit();
   }
   void SetCurrentBall(int ball) { Small("SetCurrentBall", Wire::GAME_BALL, ball); }
   void SetActivePlayer(int player) { Small("SetActivePlayer", Wire::GAME_ACTIVE_PLAYER, player); }

   void SetScore(int player, double score, int feature)
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
      if (!SmallValue("SetScore", player, op.value))
         return;
      op.score = static_cast<int64_t>(score);
      op.feature = feature;
      Op(op);
   }

   void AddMode(const char* mode) { Mode("AddMode", Wire::GAME_ADD_MODE, mode); }
   void AddModeExpiring(int seconds, const char* mode)
   {
      if (s_session == nullptr)
         return;
      if (seconds < 0)
      {
         s_session->ScriptCallIgnored("Scorbit.AddModeExpiring ignored, seconds is negative");
         return;
      }
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

private:
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

   // Range checks are the SDK's (contract §3); this only refuses what a u8 cannot carry.
   static bool SmallValue(const char* verb, int v, uint8_t& out)
   {
      if (v < 0 || v > 255)
      {
         s_session->ScriptCallIgnored("Scorbit."s + verb + " ignored, " + std::to_string(v) + " is outside 0 to 255");
         return false;
      }
      out = static_cast<uint8_t>(v);
      return true;
   }

   static void Small(const char* verb, uint8_t kind, int v)
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
   PSC_FUNCTION1(void, SetGameStarted, int)
   PSC_FUNCTION0(void, SetGameFinished)
   PSC_FUNCTION1(void, SetCurrentBall, int)
   PSC_FUNCTION1(void, SetActivePlayer, int)
   // Two members of one name: VPX picks the overload by argument count, so feature is optional.
   members.push_back({ { "SetScore", 0 }, { "void", 0 }, 2, { { "int", 0 }, { "double", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->SetScore(pArgs[0].vInt, pArgs[1].vDouble, 0); } });
   PSC_FUNCTION3(void, SetScore, int, double, int)
   members.push_back({ { "AddMode", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->AddMode(pArgs[0].vString.string); } });
   members.push_back({ { "AddModeExpiring", 0 }, { "void", 0 }, 2, { { "int", 0 }, { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->AddModeExpiring(pArgs[0].vInt, pArgs[1].vString.string); } });
   members.push_back({ { "SetModeCompleted", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->SetModeCompleted(pArgs[0].vString.string); } });
   members.push_back({ { "RemoveMode", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->RemoveMode(pArgs[0].vString.string); } });
   PSC_FUNCTION0(void, ClearModes)
   PSC_FUNCTION0(void, Commit)
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
