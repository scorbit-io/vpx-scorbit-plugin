// license:GPLv3+

#pragma once

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
// uploads is composited over the live display for a duration, or until hidden.
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

   // API thread only. 0 withdraws the override.
   void SetController(uint32_t controllerEndpointId);

   // API thread only. Payload is the WriteDmdOverlay (0x23) bytes: width,
   // height, then width*height pixels (brightness 0-15, 0x80 = transparent).
   // False if the payload is malformed or larger than the display.
   bool Upload(const uint8_t* payload, size_t size);
   // API thread only. 0 shows until Hide().
   void Show(uint32_t durationMs);
   void Hide();

   // API thread only. Demo mode: poll <dir>/overlay.bin and <dir>/overlay-ctl.bin
   // (the raw 0x23 and 0x27 payloads) and feed them through the Overlay message.
   // Empty disables.
   void SetDropDir(const std::string& dir);

private:
   void FilterSources(std::vector<DisplaySrcId>& items);
   void OnSourcesChanged();
   void Publish();
   void Withdraw();
   static void OnOverlayMsg(const unsigned int msgId, void* userData, void* msgData);
   static DisplayFrame GetRenderFrame(void* callContext);
   static DisplayFrame GetIdentifyFrame(void* callContext);
   const void* Composite(const DisplayFrame& src);
   void DropWorker();
   static void OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData);
   void DrainDrops();

   const MsgPluginAPI* const m_msgApi;
   const uint32_t m_endpointId;
   const unsigned int m_overlayMsgId;
   const unsigned int m_prepareFrameMsgId;

   std::atomic<uint32_t> m_controllerEndpointId { 0 };
   PinballPlugin::Controller::CtrlItemConsumer<DisplaySrcId> m_sources;
   PinballPlugin::Controller::CtrlItemProvider<DisplaySrcId> m_override;

   // Everything the render path touches. Held briefly by GetRenderFrame and by
   // the API-thread mutators; never across a cross-plugin call other than the
   // controller's own GetRenderFrame.
   std::mutex m_mutex;
   DisplaySrcId m_source { };
   bool m_published = false;

   struct Bitmap
   {
      uint32_t width = 0;
      uint32_t height = 0;
      std::vector<uint8_t> pixels; // as uploaded: brightness | 0x80 transparent
   };
   Bitmap m_bitmap;
   bool m_visible = false;
   std::chrono::steady_clock::time_point m_visibleUntil { };
   bool m_untilHidden = false;

   // Composited output, double-buffered so a swap never tears a frame VPX is reading.
   std::vector<float> m_lum[2];
   std::vector<uint8_t> m_rgb[2];
   unsigned int m_target = 0;
   unsigned int m_srcFrameId = 0;
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
