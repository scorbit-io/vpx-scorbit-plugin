// license:GPLv3+

#include "common.h"
#include "Scorbit.h"
#include "DmdTap.h"

#include "plugins/ControllerPlugin.h"
#include "pinmame/PinMAMEPlugin.h"

#include "nlohmann/json.hpp"

#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tchar.h>
#else
#include <dlfcn.h>
#endif

using json = nlohmann::json;
using namespace PinballPlugin::Controller;

namespace Scorbit {

LPI_IMPLEMENT_CPP

// Baked in at build time from SCORBIT_PROVIDER_KEY (environment or .env); empty
// leaves the plugin disabled until the user sets providerKey in VPinballX.ini.
#ifndef SCORBIT_DEFAULT_PROVIDER_KEY
#define SCORBIT_DEFAULT_PROVIDER_KEY ""
#endif

static constexpr double POLL_INTERVAL = 0.25;

static const MsgPluginAPI* msgApi = nullptr;
static VPXPluginAPI* vpxApi = nullptr;
static uint32_t endpointId = 0;
static unsigned int getVpxApiId = 0;

static std::unique_ptr<CtrlItemConsumer<ControllerDef>> controllers;
static std::unique_ptr<CtrlItemConsumer<StateSrcId>> stateSources;

// Raw pointer on purpose: a static unique_ptr would run ~Scorbit from
// __cxa_finalize when the host exits without unloading plugins (e.g. the
// signal handler exit path), spawning threads and calling into the already
// finalized SDK. Abnormal exits leak instead; normal unload deletes below.
static Scorbit* scorbit = nullptr;
// Same reason: ~DmdTap joins a thread and unsubscribes through the host API.
static DmdTap* dmdTap = nullptr;

static string currentRomId;
static uint32_t pinmameEndpointId = 0;

static std::atomic<bool> pollActive { false };
static bool wasGameOver = true;
static int64_t lastScores[4] = { 0, 0, 0, 0 };
static int lastPlayers = 1;

MSGPI_STRING_VAL_SETTING(setProvider, "provider", "Provider",
   "Scorbit provider id", false, "vpxplugin", 64);
MSGPI_STRING_VAL_SETTING(setEnv, "environment", "Environment",
   "production or staging", true, "staging", 32);
MSGPI_STRING_VAL_SETTING(setKey, "providerKey", "Provider key",
   "Scorbit provider key", true, SCORBIT_DEFAULT_PROVIDER_KEY, 2048);
MSGPI_STRING_VAL_SETTING(setKeyFile, "deviceKeyFile", "Device key file",
   "Per-machine key filename, relative to the pref dir", false, "scorbit_device.key", 256);
MSGPI_INT_VAL_SETTING(setLog, "logLevel", "Log level",
   "0 quiet, 1 info, 2 debug", true, 0, 2, 2);
MSGPI_STRING_VAL_SETTING(setDmdDump, "dmdDumpFile", "DMD dump file",
   "Write captured DMD frames to this file as dmddump text (one hex digit per pixel), empty to disable", false, "", 1024);

ScorbitConfig GetPluginConfig()
{
   ScorbitConfig c;
   c.provider = setProvider_Get();
   c.environment = setEnv_Get();
   c.providerKey = setKey_Get();
   c.deviceKeyFile = setKeyFile_Get();
   c.logLevel = setLog_Get();
   return c;
}

// Game states published by libpinmame from a tomlogic memory map (see
// https://github.com/tomlogic/pinmame-nvram-maps): each map entry becomes an
// INT64 state of the PMPI_GROUP_GAMESTATE source whose desc holds the JSON
// group path (e.g. "game_state\scores\Player 1"), so states are matched on
// the standardized game_state keys, not on the per-map display labels.
struct GameStates
{
   CtlResId id { };
   unsigned int nStates = 0;
   unsigned int scores[4] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
   unsigned int ball = UINT_MAX;
   unsigned int gameOver = UINT_MAX;
   unsigned int playerCount = UINT_MAX;
   unsigned int currentPlayer = UINT_MAX;
};

static GameStates states;

static void FilterControllers(std::vector<ControllerDef>& items)
{
   std::erase_if(items, [](const ControllerDef& def)
      { return def.gameId == nullptr || !std::string_view(def.gameId).starts_with(PMPI_GAMEID_PREFIX); });
}

static void FilterStateSources(std::vector<StateSrcId>& items)
{
   std::erase_if(items, [](const StateSrcId& src)
      { return src.id.endpointId != pinmameEndpointId || src.id.resId != PMPI_GROUP_GAMESTATE; });
}

static void ResolveStates()
{
   states = { };

   std::lock_guard lock(stateSources->GetListMutex());
   for (const StateSrcId& src : stateSources->GetItems())
   {
      GameStates resolved;
      resolved.id = src.id;
      resolved.nStates = src.nStates;
      unsigned int nScores = 0;
      for (unsigned int i = 0; i < src.nStates; i++)
      {
         const StateDef& def = src.stateDefs[i];
         if (def.dataFormat != CTLPI_STATE_FORMAT_INT64 || def.GetState == nullptr)
            continue;
         const std::string_view path = def.desc ? def.desc : "";
         if (path.starts_with("game_state\\scores\\"sv) && nScores < 4)
            resolved.scores[nScores++] = i;
         else if (path.starts_with("game_state\\current_ball"sv))
            resolved.ball = i;
         else if (path.starts_with("game_state\\game_over"sv))
            resolved.gameOver = i;
         else if (path.starts_with("game_state\\player_count"sv))
            resolved.playerCount = i;
         else if (path.starts_with("game_state\\current_player"sv))
            resolved.currentPlayer = i;
      }
      if (resolved.scores[0] != UINT_MAX && resolved.gameOver != UINT_MAX)
      {
         states = resolved;
         LOGI("Game states resolved: scores="s + std::to_string(nScores)
            + " ball=" + (states.ball != UINT_MAX ? "yes" : "no")
            + " playerCount=" + (states.playerCount != UINT_MAX ? "yes" : "no")
            + " currentPlayer=" + (states.currentPlayer != UINT_MAX ? "yes" : "no"));
         return;
      }
   }
}

// libpinmame leaves pResult untouched when the machine is not running or the
// memory region is unmapped, so the default value must be preset by the caller.
static int64_t ReadState(const StateSrcId& src, unsigned int index, int64_t defValue)
{
   if (index >= src.nStates)
      return defValue;
   int64_t value = defValue;
   src.stateDefs[index].GetState(src.id, index, &value);
   return value;
}

static void UpdateSession(const StateSrcId& src)
{
   const bool gameOver = ReadState(src, states.gameOver, 1) != 0;
   const int ball = static_cast<int>(ReadState(src, states.ball, 0));
   const int64_t playerCount = ReadState(src, states.playerCount, 1);
   const int players = (playerCount < 1 || playerCount > 4) ? 1 : static_cast<int>(playerCount);
   const int64_t currentPlayer = ReadState(src, states.currentPlayer, 1);
   const int player = (currentPlayer < 1 || currentPlayer > 4) ? 1 : static_cast<int>(currentPlayer);

   int64_t s[4];
   for (int i = 0; i < 4; i++)
      s[i] = ReadState(src, states.scores[i], 0);

   if (!gameOver && wasGameOver)
   {
      LOGI("Game started ("s + std::to_string(players) + " players)");
      scorbit->StartSession();
   }

   if (!gameOver)
   {
      const bool changed = s[0] != lastScores[0] || s[1] != lastScores[1]
         || s[2] != lastScores[2] || s[3] != lastScores[3];
      if (changed)
         scorbit->SendUpdate(static_cast<double>(s[0]), static_cast<double>(s[1]),
            static_cast<double>(s[2]), static_cast<double>(s[3]), ball, player, players);
      for (int i = 0; i < 4; i++)
         lastScores[i] = s[i];
      lastPlayers = players;
   }

   if (gameOver && !wasGameOver)
   {
      LOGI("Game over, p1="s + std::to_string(s[0]));
      scorbit->StopSession(static_cast<double>(s[0]), static_cast<double>(s[1]),
         static_cast<double>(s[2]), static_cast<double>(s[3]), players);
   }

   wasGameOver = gameOver;
}

static void PollStates(void*)
{
   if (!pollActive || !scorbit)
      return;

   if (states.nStates != 0)
   {
      std::lock_guard lock(stateSources->GetListMutex());
      for (const StateSrcId& src : stateSources->GetItems())
      {
         if (src.id.id == states.id.id && src.nStates == states.nStates)
         {
            UpdateSession(src);
            break;
         }
      }
   }

   msgApi->RunOnMainThread(endpointId, POLL_INTERVAL, PollStates, nullptr);
}

static void StartPoll()
{
   wasGameOver = true;
   for (int64_t& v : lastScores)
      v = 0;
   lastPlayers = 1;
   if (!pollActive.exchange(true))
      msgApi->RunOnMainThread(endpointId, POLL_INTERVAL, PollStates, nullptr);
}

static void StopPoll()
{
   if (!pollActive.exchange(false))
      return;
   if (scorbit && !wasGameOver)
      scorbit->StopSession(static_cast<double>(lastScores[0]), static_cast<double>(lastScores[1]),
         static_cast<double>(lastScores[2]), static_cast<double>(lastScores[3]), lastPlayers);
   wasGameOver = true;
}

struct MachineInfo
{
   int machineId = 0;
   string uuid;
};

static std::filesystem::path GetPluginPath()
{
#ifdef _WIN32
   HMODULE hm = nullptr;
   if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, _T("ScorbitPluginLoad"), &hm) == 0)
      return std::filesystem::path();

   std::wstring pathBuf;
   DWORD size = MAX_PATH;
   while (true)
   {
      pathBuf.resize(size);
      const DWORD length = ::GetModuleFileNameW(hm, pathBuf.data(), size);
      if (length == 0)
         return std::filesystem::path();
      if (length < size)
      {
         pathBuf.resize(length);
         break;
      }
      size *= 2;
   }
   std::filesystem::path path(pathBuf);
#else
   Dl_info info { };
   if (dladdr((void*)&GetPluginPath, &info) == 0 || !info.dli_fname)
      return std::filesystem::path();

   char pathBuf[PATH_MAX];
   if (!realpath(info.dli_fname, pathBuf))
      return std::filesystem::path();
   std::filesystem::path path(pathBuf);
#endif
   return path.empty() ? path : path.parent_path();
}

static bool LookupMachine(const string& romId, MachineInfo& info)
{
   const std::filesystem::path file = GetPluginPath() / "assets"sv / "scorbit_machines.json"sv;

   std::ifstream f(file);
   if (!f.is_open())
   {
      LOGI("No machines file at "s + file.string()
         + " - expected JSON like { \"" + romId + "\": { \"machineId\": 1582, \"uuid\": \"...\" } }");
      return false;
   }

   try
   {
      json machines;
      f >> machines;
      if (machines.is_object() && machines.contains(romId) && machines[romId].is_object())
      {
         info.machineId = machines[romId].value("machineId", 0);
         info.uuid = machines[romId].value("uuid", "");
         return info.machineId != 0;
      }
   }
   catch (const json::exception& e)
   {
      LOGE("Failed to parse "s + file.string() + ": " + e.what());
   }
   return false;
}

static void OnControllersChanged()
{
   string romId;
   uint32_t controllerEndpointId = 0;
   {
      std::lock_guard lock(controllers->GetListMutex());
      if (!controllers->GetItems().empty())
      {
         const ControllerDef& controller = controllers->GetItems().front();
         romId = string(controller.gameId).substr(strlen(PMPI_GAMEID_PREFIX));
         controllerEndpointId = controller.endpointId;
      }
   }

   if (romId == currentRomId && controllerEndpointId == pinmameEndpointId)
      return;

   StopPoll();
   delete scorbit;
   scorbit = nullptr;
   currentRomId = romId;
   pinmameEndpointId = controllerEndpointId;

   // Re-filter the state sources and the display source for the controller that is now emulating
   stateSources->SelectItems(false);
   if (dmdTap)
      dmdTap->SetController(pinmameEndpointId);

   if (romId.empty())
   {
      LOGI("Game ended"s);
      return;
   }

   MachineInfo machine;
   if (!LookupMachine(romId, machine))
   {
      LOGI("romId="s + romId + " has no Scorbit machine mapping - Scorbit idle");
      return;
   }

   LOGI("New game: romId="s + romId + " machineId=" + std::to_string(machine.machineId));

   scorbit = new Scorbit(vpxApi);
   if (scorbit->DoInit(machine.machineId, romId, machine.uuid))
      StartPoll();
}

}

using namespace Scorbit;

MSGPI_EXPORT void MSGPIAPI ScorbitPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;
   LPISetup(endpointId, msgApi);
   LOGI("Scorbit plugin loading"s);

   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);

   msgApi->RegisterSetting(endpointId, &setProvider);
   msgApi->RegisterSetting(endpointId, &setEnv);
   msgApi->RegisterSetting(endpointId, &setKey);
   msgApi->RegisterSetting(endpointId, &setKeyFile);
   msgApi->RegisterSetting(endpointId, &setLog);
   msgApi->RegisterSetting(endpointId, &setDmdDump);

   dmdTap = new DmdTap(msgApi, endpointId);
   {
      // A relative dump path is taken against the pref dir, like deviceKeyFile.
      std::filesystem::path dump = setDmdDump_Get();
      if (!dump.empty() && dump.is_relative() && vpxApi != nullptr)
      {
         VPXInfo info { };
         vpxApi->GetVpxInfo(&info);
         if (info.prefPath != nullptr)
            dump = std::filesystem::path(info.prefPath) / dump;
      }
      dmdTap->SetDumpFile(dump.string());
   }

   stateSources = std::make_unique<CtrlItemConsumer<StateSrcId>>(msgApi, endpointId, CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG,
      [](std::vector<StateSrcId>& items) { FilterStateSources(items); }, []() { ResolveStates(); });
   controllers = std::make_unique<CtrlItemConsumer<ControllerDef>>(msgApi, endpointId, CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
      [](std::vector<ControllerDef>& items) { FilterControllers(items); }, []() { OnControllersChanged(); });

   const ScorbitConfig c = GetPluginConfig();
   LOGI("Scorbit plugin loaded. provider="s + c.provider
      + " env=" + c.environment
      + " key=" + (c.providerKey.empty() ? "MISSING (Scorbit will stay disabled)"s
                                         : ("present (" + std::to_string(c.providerKey.size()) + " chars)")));

   controllers->SelectItems(false);
}

MSGPI_EXPORT void MSGPIAPI ScorbitPluginUnload()
{
   LOGI("Scorbit plugin unloading"s);

   StopPoll();
   msgApi->FlushPendingCallbacks(endpointId);

   controllers = nullptr;
   stateSources = nullptr;
   delete dmdTap;
   dmdTap = nullptr;

   delete scorbit;
   scorbit = nullptr;
   currentRomId.clear();
   pinmameEndpointId = 0;
   states = { };

   msgApi->ReleaseMsgID(getVpxApiId);
   vpxApi = nullptr;
   msgApi = nullptr;
}
