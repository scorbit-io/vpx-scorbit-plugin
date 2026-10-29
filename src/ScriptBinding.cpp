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

// One per CreateObject; every instance speaks for the same table.
class ScorbitTable final
{
public:
   PSC_IMPLEMENT_REFCOUNT()

   void Declare(const char* machineId)
   {
      if (s_session != nullptr)
         s_session->DeclareMachine(machineId != nullptr ? machineId : "");
   }
};

PSC_CLASS_START(Scorbit_Table, ScorbitTable)
   // By hand rather than PSC_FUNCTION1, whose string conversion would not survive a null.
   members.push_back({ { "Declare", 0 }, { "void", 0 }, 1, { { "string", 0 } },
      [](void* me, int, ScriptVariant* pArgs, ScriptVariant*) { static_cast<ScorbitTable*>(me)->Declare(pArgs[0].vString.string); } });
PSC_CLASS_END()

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

}

void RegisterScriptBinding(const MsgPluginAPI* msgApi, uint32_t endpointId, VpxSessionSource* session)
{
   s_msgApi = msgApi;
   s_session = session;
   s_getScriptApiId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, s_getScriptApiId, &s_scriptApi);
   if (s_scriptApi == nullptr)
   {
      LOGW("script: VPX offers no scriptable API, so "s + PROG_ID + " is unavailable to table scripts");
      return;
   }
   RegisterScorbit_Table([](ScriptClassDef* scd) { s_scriptApi->RegisterScriptClass(scd); });
   s_scriptApi->SubmitTypeLibrary(endpointId);
   Scorbit_Table_SCD->CreateObject = []() -> void* { return new ScorbitTable(); };
   s_scriptApi->SetCOMObjectOverride(PROG_ID, Scorbit_Table_SCD);
}

void UnregisterScriptBinding()
{
   s_session = nullptr;
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
