// license:GPLv3+

#pragma once

#include "OverlayState.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <deque>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Scorbit
{

// DMD overlay, drawn the way the hardware probe draws it: a bitmap the daemon
// uploads is composited over the live display for a duration, or until hidden, each
// value shaded as the probe on that platform shows it (OverlayState.h).
//
// The plugin API has no draw callback, so the overlay is a render-frame
// provider: this publishes its own display source that overrides the emulating
// controller's, and serves the controller's render frame with the cached bitmap
// composited over it. Identify frames pass through untouched, so colorizers and
// the frame tap keep seeing the real display. Nothing in the render path blocks
// on anything but a short local lock.
class DmdOverlay final
{
public:
   DmdOverlay(const MsgPluginAPI* msgApi, uint32_t endpointId);
   ~DmdOverlay();

   // API thread only. Stops the drop worker and every subscription but leaves the
   // override published, broadcasting nothing. Used alone on the quit path, where
   // the object is then left to the OS (SB-5801); the destructor calls it too.
   void Detach();

   // API thread only. 0 withdraws the override.
   void SetController(uint32_t controllerEndpointId);

   // Any thread. The controller publishes a display this overrides, so there is
   // something to composite on.
   bool HasSurface() const { return m_surface; }

   // Any thread, and silent: the socket worker calls these, and nothing may log off
   // the API thread. The payload is the WriteDmdOverlay (0x23) bytes: width, height,
   // then width*height pixels, bit 7 transparent, the low bits platform bitplane bits.
   Overlay::State::Upload Store(const uint8_t* payload, size_t size);
   // The 0x27 control. Enabling restarts the timer, 0 meaning until hidden.
   void Control(bool enable, uint32_t durationMs);
   // Session end: hidden, with no bitmap and no timer.
   void Clear();

   // API thread only. Demo mode: poll <dir>/overlay.bin and <dir>/overlay-ctl.bin
   // (the raw 0x23 and 0x27 payloads) and feed them through the Overlay message.
   // Empty disables.
   void SetDropDir(const std::string& dir);

private:
   void FilterSources(std::vector<DisplaySrcId>& items);
   void OnSourcesChanged();
   void Publish();
   void Withdraw();
   // Source loss: stop forwarding to the controller now, broadcast the withdrawal
   // on the next main-thread turn. Losing the source happens inside the controller's
   // own teardown broadcast; a colorizer that restarts its thread on our change
   // there then deadlocks joining it (SB-5801).
   void WithdrawDeferred();
   static void MSGPIAPI OnDeferredWithdraw(void* userData);
   static void OnOverlayMsg(const unsigned int msgId, void* userData, void* msgData);
   static DisplayFrame GetRenderFrame(void* callContext);
   static DisplayFrame GetIdentifyFrame(void* callContext);
   const void* Composite(const DisplayFrame& src);
   void DropWorker();
   static void OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData);
   void DrainDrops();
   // PinMAME answers GetMachineState only once emulation runs, so this asks every frame until it does.
   void QueryPlatform();

   const MsgPluginAPI* const m_msgApi;
   const uint32_t m_endpointId;
   const unsigned int m_overlayMsgId;
   const unsigned int m_prepareFrameMsgId;
   const unsigned int m_machineStateMsgId;

   bool m_detached = false;
   bool m_withdrawPending = false; // API thread only
   bool m_platformKnown = false;   // API thread only
   std::atomic<bool> m_surface { false };
   std::atomic<uint32_t> m_controllerEndpointId { 0 };
   PinballPlugin::Controller::CtrlItemConsumer<DisplaySrcId> m_sources;
   PinballPlugin::Controller::CtrlItemProvider<DisplaySrcId> m_override;

   // Everything the render path touches. Held briefly by GetRenderFrame and by
   // the API-thread mutators; never across a cross-plugin call other than the
   // controller's own GetRenderFrame.
   std::mutex m_mutex;
   DisplaySrcId m_source { };
   bool m_published = false;
   // What the override advertises. m_source is cleared as soon as the controller's display
   // goes, but the override stays listed until the deferred withdrawal, and consumers keep
   // reading it in the format it was published with.
   unsigned int m_pubFrameFormat = 0;
   bool m_pubIdentify = false;

   Overlay::State m_state;
   Overlay::Platform m_platform = Overlay::Platform::Unknown;

   // Composited output, double-buffered so a swap never tears a frame VPX is reading.
   std::vector<float> m_lum[2];
   std::vector<uint8_t> m_rgb[2];
   unsigned int m_target = 0;
   unsigned int m_srcFrameId = 0;
   // Served while withdrawn but still listed: identify frames are never null by contract,
   // and repeating the last id tells consumers nothing new arrived.
   std::vector<uint8_t> m_idBlank;
   unsigned int m_lastIdFrameId = 0;
   unsigned int m_outFrameId = 0;
   bool m_lastComposited = false;
   uint64_t m_compositedFrames = 0;

   // Demo file drop. The worker only reads files and queues them; the queue is
   // drained on the API thread from OnPrepareFrame, so the demo never posts
   // callbacks from a worker and nothing is applied before the player runs.
   std::mutex m_dropMutex;
   std::filesystem::path m_dropDir;
   std::atomic<bool> m_dropRunning { false };
   std::thread m_dropThread;
   struct DropEvent
   {
      int op;
      std::vector<uint8_t> bytes;
   };
   std::deque<DropEvent> m_dropQueue;
};

}
