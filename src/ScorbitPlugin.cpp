// license:GPLv3+

#include "common.h"
#include "DmdTap.h"
#include "DmdOverlay.h"
#include "QuitHook.h"
#include "SocketWorker.h"
#include "VpxSessionSource.h"

#include "plugins/ControllerPlugin.h"
#include "pinmame/PinMAMEPlugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace PinballPlugin::Controller;

namespace Scorbit {

LPI_IMPLEMENT_CPP

// Identity the plugin sends in Hello, baked in by CMakeLists.txt.
#ifndef SCORBIT_PLUGIN_VERSION
#define SCORBIT_PLUGIN_VERSION "0.0.0"
#endif
#ifndef SCORBIT_VPX_PLUGIN_API_COMMIT
#define SCORBIT_VPX_PLUGIN_API_COMMIT ""
#endif

// The socket worker cannot log from its own thread, so its lines are drained
// here, along with the Declare and overlay work the API thread owns.
static constexpr double LOG_DRAIN_INTERVAL = 0.25;

static const MsgPluginAPI* msgApi = nullptr;
static VPXPluginAPI* vpxApi = nullptr;
static uint32_t endpointId = 0;
static unsigned int getVpxApiId = 0;

static std::unique_ptr<CtrlItemConsumer<ControllerDef>> controllers;

// Raw pointers on purpose: a static unique_ptr would run the destructors from
// __cxa_finalize when the host exits without unloading plugins (e.g. the signal
// handler exit path). ~DmdTap joins a thread and unsubscribes through the host API,
// and ~SocketWorker joins the transport thread. Abnormal exits leak instead; normal
// unload deletes below.
static DmdTap* dmdTap = nullptr;
static DmdOverlay* dmdOverlay = nullptr;
static VpxSessionSource* vpxSession = nullptr;
static SocketWorker* socketWorker = nullptr;
static std::atomic<bool> logDrainActive { false };

static string currentRomId;
static uint32_t pinmameEndpointId = 0;

MSGPI_STRING_VAL_SETTING(setDmdDump, "dmdDumpFile", "DMD dump file",
   "Write captured DMD frames to this file as dmddump text (one hex digit per pixel), empty to disable", false, "", 1024);
MSGPI_STRING_VAL_SETTING(setOverlayDrop, "overlayDropDir", "Overlay drop directory",
   "Demo: watch this directory for overlay.bin / overlay-ctl.bin (raw probe payloads) and display them, empty to disable", false, "", 1024);
MSGPI_STRING_VAL_SETTING(setSocketPath, "socketPath", "Scorbit daemon socket",
   "Local socket the Scorbit daemon listens on, empty for the per-user default", false, "", 1024);
MSGPI_BOOL_VAL_SETTING(setShowStatus, "ShowStatus", "Show status",
   "Show Scorbit status notifications, in a session or out of one", true, true);

static void FilterControllers(std::vector<ControllerDef>& items)
{
   std::erase_if(items, [](const ControllerDef& def)
      { return def.gameId == nullptr || !std::string_view(def.gameId).starts_with(PMPI_GAMEID_PREFIX); });
}

// API thread. Reschedules itself for the plugin's lifetime, so a line the
// transport thread queued is written out whether or not a table is running.
static void DrainSocketLog(void*)
{
   if (!logDrainActive)
      return;
   if (vpxSession != nullptr)
   {
      vpxSession->DrainLog();
      // Display changes while no frame renders (table chooser, a paused game) still re-declare.
      vpxSession->UpdateDeclare();
   }
   if (dmdOverlay != nullptr)
      dmdOverlay->DrainPending();
   msgApi->RunOnMainThread(endpointId, LOG_DRAIN_INTERVAL, DrainSocketLog, nullptr);
}

// The daemon is the Scorbitron: it reads the game from memory and frames and talks to
// Scorbit. The plugin only tells it which ROM is running and where its display is.
static void OnControllersChanged()
{
   string romId;
   uint32_t controllerEndpointId = 0;
   controllers->With([&](const std::vector<ControllerDef>& items)
   {
      if (!items.empty())
      {
         const ControllerDef& controller = items.front();
         romId = string(controller.gameId).substr(strlen(PMPI_GAMEID_PREFIX));
         controllerEndpointId = controller.endpointId;
      }
   });

   if (romId == currentRomId && controllerEndpointId == pinmameEndpointId)
      return;

   currentRomId = romId;
   pinmameEndpointId = controllerEndpointId;

   // Re-filter the display source for the controller that is now emulating
   if (dmdTap)
      dmdTap->SetController(pinmameEndpointId);
   if (dmdOverlay)
      dmdOverlay->SetController(pinmameEndpointId);
   if (vpxSession)
      vpxSession->SetRomId(romId);

   LOGI(romId.empty() ? "Game ended"s : "New game: romId="s + romId);
}

}

using namespace Scorbit;

MSGPI_EXPORT void MSGPIAPI ScorbitPluginUnload();

// VPX's application quit exits without unloading plugins, so static teardown would run
// against a half-finished process. Unload while the process is still whole (SB-5039).
static bool unloadingAtQuit = false;

static void UnloadOnQuit()
{
   if (msgApi == nullptr)
      return;
   LOGI("Scorbit plugin unloading at quit: VPX did not unload it"s);
   unloadingAtQuit = true;
   ScorbitPluginUnload();
}

MSGPI_EXPORT void MSGPIAPI ScorbitPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;
   unloadingAtQuit = false;
   LPISetup(endpointId, msgApi);
   LOGI("Scorbit plugin loading"s);
   InstallQuitHook(UnloadOnQuit);

   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);

   msgApi->RegisterSetting(endpointId, &setDmdDump);
   msgApi->RegisterSetting(endpointId, &setOverlayDrop);
   msgApi->RegisterSetting(endpointId, &setSocketPath);
   msgApi->RegisterSetting(endpointId, &setShowStatus);

   dmdTap = new DmdTap(msgApi, endpointId);
   {
      // A relative dump path is taken against the pref dir.
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

   dmdOverlay = new DmdOverlay(msgApi, endpointId);
   {
      std::filesystem::path drop = setOverlayDrop_Get();
      if (!drop.empty() && drop.is_relative() && vpxApi != nullptr)
      {
         VPXInfo info { };
         vpxApi->GetVpxInfo(&info);
         if (info.prefPath != nullptr)
            drop = std::filesystem::path(info.prefPath) / drop;
      }
      dmdOverlay->SetDropDir(drop.string());
   }

   // The daemon listens and the plugin connects, so this starts whether or not
   // a daemon is running and retries once a second until one is.
   vpxSession = new VpxSessionSource(msgApi, endpointId, vpxApi, *dmdTap, *dmdOverlay, &setShowStatus_Get);
   {
      SocketWorkerConfig cfg;
      cfg.socketPath = setSocketPath_Get();
      cfg.pluginVersion = SCORBIT_PLUGIN_VERSION;
      cfg.pluginApiCommit = SCORBIT_VPX_PLUGIN_API_COMMIT;
      socketWorker = new SocketWorker(*vpxSession, cfg,
         [](int level, const std::string& message) { vpxSession->PushLog(level, message); },
         &vpxSession->Memory(), vpxSession, &vpxSession->Status());
      socketWorker->Start();
   }
   logDrainActive = true;
   msgApi->RunOnMainThread(endpointId, LOG_DRAIN_INTERVAL, DrainSocketLog, nullptr);

   controllers = std::make_unique<CtrlItemConsumer<ControllerDef>>(msgApi, endpointId, CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
      [](std::vector<ControllerDef>& items) { FilterControllers(items); }, nullptr, []() { OnControllersChanged(); });

   LOGI("Scorbit plugin loaded "s + SCORBIT_PLUGIN_VERSION);

   controllers->Subscribe();
}

MSGPI_EXPORT void MSGPIAPI ScorbitPluginUnload()
{
   if (msgApi == nullptr)
      return;
   LOGI("Scorbit plugin unloading"s);
   RemoveQuitHook();
   // Withdrawing the override broadcasts a display-source change. At quit, a colorizer
   // restarts its thread on it and exit() then deadlocks joining that thread (SB-5801).
   // Detach first: unsubscribing the controllers below would otherwise withdraw it
   // through OnControllersChanged. The process is ending, so the override stays
   // published and the object is left to the OS.
   if (unloadingAtQuit)
      dmdOverlay->Detach();

   logDrainActive = false;
   // Joins the transport thread, so nothing reads vpxSession or dmdTap after
   // this returns. Bye goes out from the worker as it leaves its serve loop.
   delete socketWorker;
   socketWorker = nullptr;
   msgApi->FlushPendingCallbacks(endpointId);

   controllers->Unsubscribe();
   controllers = nullptr;
   delete vpxSession;
   vpxSession = nullptr;
   if (!unloadingAtQuit)
      delete dmdOverlay;
   dmdOverlay = nullptr;
   delete dmdTap;
   dmdTap = nullptr;

   currentRomId.clear();
   pinmameEndpointId = 0;

   // Unsubscribing the controllers above can queue a deferred overlay withdrawal after
   // the first flush. Run it now, while this library is still loaded; it finds the
   // overlay gone and returns.
   msgApi->FlushPendingCallbacks(endpointId);

   msgApi->ReleaseMsgID(getVpxApiId);
   vpxApi = nullptr;
   msgApi = nullptr;
}
