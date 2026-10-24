// license:GPLv3+

#pragma once

#include "OverlayCache.h"
#include "OverlayState.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
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
// The plugin API has no draw callback, so the overlay is delivered as display-source
// overrides that every downstream consumer picks up as if it were original data
// (docs/slice4-internals.md):
// - The display list is answered from the plugin's own subscription, made at load and
//   released only at unload, so its overrides are listed ahead of every source that
//   subscribes later, and consumers that take the first match pick them.
// - It overrides each leaf of the override chain that starts at the controller's
//   display, never beside another override of the same source: the display itself on
//   an uncoloured table, every colouriser output on a coloured one.
// - Only an override of the controller's display passes identify frames through; one
//   of a colouriser's output exposes none, or the colouriser could select it as its
//   input and close a cycle.
// The render path reads the overlay through a seqlock and never waits on a writer.
class DmdOverlay final
{
public:
   DmdOverlay(const MsgPluginAPI* msgApi, uint32_t endpointId);
   ~DmdOverlay();

   // API thread only. Stops the drop worker and every subscription but the display
   // list's, and leaves the overrides listed and dark, broadcasting nothing. Used alone
   // on the quit path, where the object is then left to the OS (SB-5801); the
   // destructor calls it too.
   void Detach();

   // API thread only. 0 withdraws every override.
   void SetController(uint32_t controllerEndpointId);

   // Any thread. Some override is live, so there is something to composite on.
   bool HasSurface() const { return m_surface; }
   // Any thread. Moves whenever the overrides change, which must re-Declare.
   uint64_t SurfaceGeneration() const { return m_surfaceGeneration; }

   // Socket worker, and silent: nothing may log off the API thread. The payload is the
   // WriteDmdOverlay (0x23) bytes: width, height, then width*height pixels, bit 7
   // transparent, the low bits platform bitplane bits.
   Overlay::State::Upload Store(const uint8_t* payload, size_t size) { return m_cache.Store(payload, size); }
   // The 0x27 control. Enabling restarts the timer, 0 meaning until hidden.
   void Control(bool enable, uint32_t durationMs);
   // Session end: hidden, with no bitmap and no timer.
   void Clear() { m_cache.Clear(); }

   // API thread only. Demo mode: poll <dir>/overlay.bin and <dir>/overlay-ctl.bin
   // (the raw 0x23 and 0x27 payloads) and feed them through the Overlay message.
   // Empty disables.
   void SetDropDir(const std::string& dir);

private:
   // A source to override, and how.
   struct Target
   {
      DisplaySrcId source { };
      bool identify = false;      // the controller's display: identify frames pass through
      uint32_t baseWidth = 0;     // the controller's display, where the probe places the overlay
      uint32_t baseHeight = 0;
      bool sixteenShades = false;

      bool operator==(const Target& o) const
      {
         return source == o.source && identify == o.identify && baseWidth == o.baseWidth && baseHeight == o.baseHeight
            && sixteenShades == o.sixteenShades;
      }
   };

   // One listed override. Its address is its callContext, so it never moves; render
   // threads touch it only under its own mutex, which no writer of the overlay takes.
   struct Entry
   {
      Target target;
      DisplaySrcId listed { };
      DmdOverlay* owner = nullptr;
      size_t bytesPerPixel = 0;

      std::mutex mutex;
      bool live = true;           // false once the target may be gone: our own buffers only
      std::vector<uint8_t> out[2];
      std::vector<uint8_t> dark;  // served once not live
      unsigned int next = 0;      // the buffer the next composite writes
      unsigned int srcFrameId = 0;
      unsigned int outFrameId = 0;
      bool lastComposited = false;
      Overlay::Record record;
      Overlay::Record scratch;
      uint64_t recordSequence = 0;
      int platform = -1;
      std::vector<uint8_t> idBlank;
      unsigned int lastIdFrameId = 0;
   };

   static void OnGetDisplays(const unsigned int msgId, void* userData, void* msgData);
   void FilterSources(std::vector<DisplaySrcId>& items);
   void OnSourcesChanged();
   // The overrides the current display list calls for (the "never a sibling" rule).
   std::vector<Target> Desired() const;
   bool NeedsApply() const;
   // Stops calling any target no longer listed exactly as it was. API thread, synchronous.
   void DarkenStale();
   // Broadcasts are deferred to the next main-thread turn: a source change arrives inside
   // another plugin's broadcast, a controller teardown included, and a colourizer that
   // restarts its thread on our change there deadlocks joining it.
   void ScheduleApply();
   static void MSGPIAPI OnDeferredApply(void* userData);
   void Apply();
   void UpdateSurface();
   void Broadcast();
   std::unique_ptr<Entry> MakeEntry(const Target& target);

   static void OnOverlayMsg(const unsigned int msgId, void* userData, void* msgData);
   static DisplayFrame GetRenderFrame(void* callContext);
   static DisplayFrame GetIdentifyFrame(void* callContext);
   const void* Composite(Entry& e, const DisplayFrame& src);
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
   const unsigned int m_getDisplaysMsgId;
   const unsigned int m_displaysChangedMsgId;

   // API thread only.
   bool m_detached = false;
   bool m_applyPending = false;
   bool m_platformKnown = false;
   uint16_t m_nextResId = 1;
   std::vector<std::unique_ptr<Entry>> m_entries;   // what the display list carries
   struct Retired
   {
      std::unique_ptr<Entry> entry;
      std::chrono::steady_clock::time_point at;
   };
   std::vector<Retired> m_retired;                  // withdrawn; freed once RETIRE_AFTER old

   std::atomic<uint32_t> m_controllerEndpointId { 0 };
   std::atomic<int> m_platform { static_cast<int>(Overlay::Platform::Unknown) };
   std::atomic<bool> m_surface { false };
   std::atomic<uint64_t> m_surfaceGeneration { 0 };
   std::atomic<uint64_t> m_compositedFrames { 0 };
   PinballPlugin::Controller::CtrlItemConsumer<DisplaySrcId> m_sources; // every source but ours

   Overlay::Cache m_cache;

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
