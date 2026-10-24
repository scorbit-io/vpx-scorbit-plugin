// license:GPLv3+

#pragma once

#include "SeqLock.h"

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
// through a seqlock, so a reader never observes a frame torn across the poll and
// never waits on the worker.
class DmdTap final
{
public:
   struct Frame
   {
      unsigned int frameId = 0;
      unsigned int width = 0;
      unsigned int height = 0;
      unsigned int shades = 0;     // 4 or 16
      uint64_t generation = 0;     // DmdTap source generation that produced this frame
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

   // Any thread. Copies the latest complete frame through the caller's scratch buffer,
   // which it keeps between calls; false if none was captured yet, or the copy tore.
   bool GetLatest(Frame& out, std::vector<uint8_t>& scratch) const;

   // Any thread. A frame has been captured, so a false GetLatest means a torn copy.
   bool HasFrame() const { return m_published.Sequence() != 0; }

   // Any thread. False means no conforming display source is selected, in which
   // case no frame will ever be produced and the caller must fail closed.
   bool HasSource() const { return m_hasSource; }

   // Any thread. Bumped on every source change, including a replacement whose
   // geometry is identical.
   // Never waits: the worker holds m_sourceMutex across a frame read and copy.
   uint64_t SourceGeneration() const { return m_sourceGeneration; }

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
   // Bumped under m_sourceMutex on every source change, and read without it.
   std::atomic<uint64_t> m_sourceGeneration { 0 };
   std::atomic<uint64_t> m_skipped { 0 };

   // The largest display any PinMAME driver publishes is 192x64; this leaves room.
   static constexpr size_t MAX_PIXELS = 256 * 128;
   struct Header
   {
      uint32_t frameId;
      uint32_t width;
      uint32_t height;
      uint32_t shades;
      uint64_t generation;
   };
   void Publish(const Frame& frame);

   Frame m_back;                    // worker only
   std::vector<uint8_t> m_record;   // worker only: header then pixels, as published
   SeqLock<sizeof(Header) + MAX_PIXELS> m_published;

   std::mutex m_dumpMutex;
   FILE* m_dump = nullptr;
   std::string m_dumpPath;
   uint64_t m_dumped = 0;
   std::vector<char> m_line;

   std::atomic<bool> m_running { false };
   std::thread m_thread;
};

}
