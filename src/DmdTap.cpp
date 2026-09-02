// license:GPLv3+

#include "common.h"
#include "DmdTap.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <pthread.h>
#endif

using namespace PinballPlugin::Controller;

namespace Scorbit
{

// PinMAME evaluates DMD frames at 60 FPS, so nothing new appears faster than this.
static constexpr auto POLL_PERIOD = std::chrono::microseconds(16666);

// Flush the dump this often so a crash loses at most a short tail.
static constexpr uint64_t DUMP_FLUSH_INTERVAL = 100;

static void NameThisThread(const char* name)
{
#if defined(_WIN32)
   wchar_t wname[64];
   if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64) > 0)
      SetThreadDescription(GetCurrentThread(), wname);
#elif defined(__APPLE__)
   pthread_setname_np(name);
#elif defined(__linux__) || defined(__ANDROID__)
   pthread_setname_np(pthread_self(), name);
#else
   (void)name;
#endif
}

DmdTap::DmdTap(const MsgPluginAPI* msgApi, uint32_t endpointId)
   : m_sources(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
        [this](std::vector<DisplaySrcId>& items) { FilterSources(items); }, nullptr, [this]() { OnSourcesChanged(); })
{
   m_running = true;
   m_thread = std::thread(&DmdTap::Worker, this);
   m_sources.Subscribe();
}

DmdTap::~DmdTap()
{
   m_running = false;
   if (m_thread.joinable())
      m_thread.join();
   m_sources.Unsubscribe();
   SetDumpFile("");
}

void DmdTap::SetController(uint32_t controllerEndpointId)
{
   m_controllerEndpointId = controllerEndpointId;
   m_sources.Refresh();
}

void DmdTap::FilterSources(std::vector<DisplaySrcId>& items)
{
   // Only the display the emulating controller publishes itself. Sources from
   // other endpoints are never consulted, even when they declare themselves as
   // overriding the controller's display: a colorizer's or upscaler's frames
   // are not what templates were trained on.
   //
   // GetIdentifyFrame is optional on a source. A source without it does not
   // conform and is skipped, and if nothing conforms the tap fails closed.
   const uint32_t controller = m_controllerEndpointId;
   DisplaySrcId selected { };
   for (const DisplaySrcId& item : items)
   {
      if (controller == 0 || item.id.endpointId != controller || item.GetIdentifyFrame == nullptr)
         continue;
      if (item.identifyFormat != CTLPI_DISPLAY_ID_FORMAT_BITPLANE2 && item.identifyFormat != CTLPI_DISPLAY_ID_FORMAT_BITPLANE4)
         continue;
      // A controller may publish more than one display; take the largest.
      if (selected.id.id == 0 || item.width * item.height > selected.width * selected.height)
         selected = item;
   }
   items.clear();
   if (selected.id.id != 0)
      items.push_back(selected);
}

void DmdTap::OnSourcesChanged()
{
   const DisplaySrcId selected = m_sources.With([](const std::vector<DisplaySrcId>& items) { return items.empty() ? DisplaySrcId { } : items.front(); });

   std::lock_guard lock(m_sourceMutex);
   if (selected.id.id == 0)
   {
      if (m_hasSource)
         LOGI("DMD tap: display source lost, no frames will be captured (skipped "s + std::to_string(m_skipped) + " so far)");
      else if (m_controllerEndpointId != 0)
         LOGI("DMD tap: controller publishes no conforming display source (no identify frames), no frames will be captured"s);
      m_source = { };
      m_hasSource = false;
      m_sourceGeneration++;
      return;
   }

   const DisplaySrcId& src = selected;
   if (m_hasSource && m_source == src)
      return;
   m_source = src;
   m_hasSource = true;
   m_sourceGeneration++;
   LOGI("DMD tap: source selected [endpointId="s + std::to_string(src.id.endpointId) + "." + std::to_string(src.id.resId) + ", " + std::to_string(src.width) + "x"
      + std::to_string(src.height) + ", " + (src.identifyFormat == CTLPI_DISPLAY_ID_FORMAT_BITPLANE4 ? "16" : "4") + " shades]");
}

void DmdTap::Worker()
{
   NameThisThread("Scorbit.DmdTap");
   uint64_t lastGeneration = 0;
   unsigned int lastFrameId = 0;
   bool haveLast = false;

   // Absolute deadlines, so the copy and dump work does not stretch the period
   // and let the poll drift behind a 60 FPS producer.
   auto next = std::chrono::steady_clock::now();
   while (m_running)
   {
      next += POLL_PERIOD;
      std::this_thread::sleep_until(next);

      {
         std::lock_guard lock(m_sourceMutex);
         if (!m_hasSource)
         {
            haveLast = false;
            continue;
         }
         const DisplaySrcId& src = m_source;
         if (m_sourceGeneration != lastGeneration)
         {
            // A replaced source may restart its frame counter at the old value,
            // so a source change resets the duplicate check even if the id matches.
            lastGeneration = m_sourceGeneration;
            haveLast = false;
         }

         // GetIdentifyFrame is documented thread safe. The buffer it returns is
         // owned by the source and may be rewritten while it is being copied, so
         // frameId is read again after the copy and a moved frame is discarded:
         // the duplicate check downstream would not catch a torn frame, and a
         // torn frame can decode as a real score. This assumes the producer
         // writes the pixels before it advances frameId (true of PinMAME and
         // the in-tree renderers); a producer that does the reverse can still
         // tear undetected.
         const DisplayFrame frame = src.GetIdentifyFrame(src.callContext);
         if (frame.frame == nullptr)
            continue;
         if (haveLast && frame.frameId == lastFrameId)
            continue;

         const size_t n = static_cast<size_t>(src.width) * src.height;
         m_back.pixels.resize(n);
         std::memcpy(m_back.pixels.data(), frame.frame, n);

         const DisplayFrame check = src.GetIdentifyFrame(src.callContext);
         if (check.frameId != frame.frameId)
            continue;

         // The source increments frameId once per frame, so a gap is a frame
         // that was produced and never seen.
         if (haveLast && frame.frameId - lastFrameId > 1)
            m_skipped += frame.frameId - lastFrameId - 1;
         lastFrameId = frame.frameId;
         haveLast = true;
         m_back.frameId = frame.frameId;
         m_back.width = src.width;
         m_back.height = src.height;
         m_back.shades = src.identifyFormat == CTLPI_DISPLAY_ID_FORMAT_BITPLANE4 ? 16 : 4;
      }

      Dump(m_back);

      {
         std::lock_guard lock(m_frameMutex);
         std::swap(m_front, m_back);
         m_hasFrame = true;
      }
   }
}

bool DmdTap::GetLatest(Frame& out) const
{
   std::lock_guard lock(m_frameMutex);
   if (!m_hasFrame)
      return false;
   out = m_front;
   return true;
}

void DmdTap::SetDumpFile(const std::string& path)
{
   std::lock_guard lock(m_dumpMutex);
   if (m_dump != nullptr)
   {
      fclose(m_dump);
      m_dump = nullptr;
      LOGI("DMD tap: closed "s + m_dumpPath + " after " + std::to_string(m_dumped) + " frames, " + std::to_string(m_skipped) + " skipped");
   }
   m_dumpPath = path;
   m_dumped = 0;
   if (path.empty())
      return;
   m_dump = fopen(path.c_str(), "wb");
   if (m_dump == nullptr)
      LOGE("DMD tap: cannot open dump file "s + path);
   else
      LOGI("DMD tap: dumping frames to "s + path);
}

void DmdTap::Dump(const Frame& frame)
{
   std::lock_guard lock(m_dumpMutex);
   if (m_dump == nullptr)
      return;

   static constexpr char digits[] = "0123456789abcdef";
   m_line.resize(frame.width + 1);
   m_line[frame.width] = '\n';
   const uint8_t* px = frame.pixels.data();
   for (unsigned int y = 0; y < frame.height; y++, px += frame.width)
   {
      for (unsigned int x = 0; x < frame.width; x++)
         m_line[x] = digits[px[x] & 0x0F];
      fwrite(m_line.data(), 1, m_line.size(), m_dump);
   }
   fputc('\n', m_dump);

   if (++m_dumped % DUMP_FLUSH_INTERVAL == 0)
      fflush(m_dump);
}

}
