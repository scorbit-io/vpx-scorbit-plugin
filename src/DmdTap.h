// license:GPLv3+

#pragma once

#include "plugins/ControllerPlugin.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Scorbit
{

// Frame tap over the controller display API.
//
// Sources frames only from the display the emulating controller publishes
// itself, so colorizers and upscalers on other endpoints are never consulted.
// That also excludes the alphanumeric-to-DMD renderer, which publishes on its
// own endpoint: segment-display games fail closed by design, since no template
// was ever trained on a synthesised DMD.
// Frames come from GetIdentifyFrame, which yields one byte per pixel holding
// the discrete shade index (0-3 for BITPLANE2, 0-15 for BITPLANE4) rather
// than rendered luminance. A worker polls frameId and publishes each new frame
// through a double buffer, so a reader never observes a frame torn across the
// poll.
class DmdTap final
{
public:
   struct Frame
   {
      unsigned int frameId = 0;
      unsigned int width = 0;
      unsigned int height = 0;
      unsigned int shades = 0;     // 4 or 16
      std::vector<uint8_t> pixels; // width * height shade indices, row major
   };

   DmdTap(const MsgPluginAPI* msgApi, uint32_t endpointId);
   ~DmdTap();

   // API thread only. 0 detaches from any source.
   void SetController(uint32_t controllerEndpointId);

   // API thread only. Empty disables. Writes every captured frame as dmddump
   // text: one line per row, one hex digit per pixel, blank line after each
   // frame. Truncates on open.
   void SetDumpFile(const std::string& path);

   // Any thread. Copies the latest complete frame; false if none captured yet.
   bool GetLatest(Frame& out) const;

   // Any thread. False means no conforming display source is selected, in which
   // case no frame will ever be produced and the caller must fail closed.
   bool HasSource() const { return m_hasSource; }

   // Any thread. Frames the source produced that the tap did not capture,
   // counted from gaps in frameId. A poll can miss a frame when the host
   // stalls the worker for longer than one frame period.
   uint64_t SkippedFrames() const { return m_skipped; }

private:
   void FilterSources(std::vector<DisplaySrcId>& items);
   void OnSourcesChanged();
   void Worker();
   void Dump(const Frame& frame);

   std::atomic<uint32_t> m_controllerEndpointId { 0 };
   PinballPlugin::Controller::CtrlItemConsumer<DisplaySrcId> m_sources;

   // Selected source. Held while the worker reads a frame, so a source change
   // on the API thread never interleaves with a read in flight.
   mutable std::mutex m_sourceMutex;
   DisplaySrcId m_source { };
   std::atomic<bool> m_hasSource { false };
   uint64_t m_sourceGeneration = 0; // under m_sourceMutex; bumped on every source change
   std::atomic<uint64_t> m_skipped { 0 };

   // Double buffer. m_back is written by the worker only; m_front is swapped
   // in under m_frameMutex and read under it by GetLatest.
   mutable std::mutex m_frameMutex;
   Frame m_front;
   Frame m_back;
   bool m_hasFrame = false;

   std::mutex m_dumpMutex;
   FILE* m_dump = nullptr;
   std::string m_dumpPath;
   uint64_t m_dumped = 0;
   std::vector<char> m_line;

   std::atomic<bool> m_running { false };
   std::thread m_thread;
};

}
