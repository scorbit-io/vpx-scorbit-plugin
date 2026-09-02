// license:GPLv3+

#include "common.h"
#include "DmdOverlay.h"
#include "ScorbitPluginAPI.h"

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace PinballPlugin::Controller;

namespace Scorbit
{

static constexpr auto DROP_POLL = std::chrono::milliseconds(100);

// Minimal SHA-256 so the plugin can log the same digest the daemon prints for a payload.
namespace
{
struct Sha256
{
   uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
   uint8_t buf[64];
   size_t bufLen = 0;
   uint64_t total = 0;

   static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

   void block(const uint8_t* p)
   {
      static constexpr uint32_t k[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
         0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
         0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
         0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee,
         0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
      uint32_t w[64];
      for (int i = 0; i < 16; i++)
         w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
      for (int i = 16; i < 64; i++)
      {
         const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
         const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
         w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; i++)
      {
         const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
         const uint32_t ch = (e & f) ^ (~e & g);
         const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
         const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
         const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
         const uint32_t t2 = S0 + maj;
         hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
   }

   void update(const uint8_t* p, size_t n)
   {
      total += n;
      while (n > 0)
      {
         const size_t take = std::min(n, sizeof(buf) - bufLen);
         std::memcpy(buf + bufLen, p, take);
         bufLen += take; p += take; n -= take;
         if (bufLen == sizeof(buf))
         {
            block(buf);
            bufLen = 0;
         }
      }
   }

   std::string hex()
   {
      const uint64_t bits = total * 8;
      uint8_t pad = 0x80;
      update(&pad, 1);
      uint8_t zero = 0;
      while (bufLen != 56)
         update(&zero, 1);
      uint8_t len[8];
      for (int i = 0; i < 8; i++)
         len[i] = (uint8_t)(bits >> (56 - 8 * i));
      update(len, 8);
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      for (uint32_t v : h)
         for (int i = 28; i >= 0; i -= 4)
            out += digits[(v >> i) & 0xF];
      return out;
   }
};

std::string Sha256Hex(const uint8_t* p, size_t n)
{
   Sha256 s;
   s.update(p, n);
   return s.hex();
}
}

DmdOverlay::DmdOverlay(const MsgPluginAPI* msgApi, uint32_t endpointId)
   : m_msgApi(msgApi)
   , m_endpointId(endpointId)
   , m_overlayMsgId(msgApi->GetMsgID(SCORBIT_NAMESPACE, SCORBIT_MSG_OVERLAY))
   , m_prepareFrameMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME))
   , m_sources(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
        [this](std::vector<DisplaySrcId>& items) { FilterSources(items); }, nullptr, [this]() { OnSourcesChanged(); })
   , m_override(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG)
{
   m_msgApi->SubscribeMsg(m_endpointId, m_overlayMsgId, OnOverlayMsg, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_prepareFrameMsgId, OnPrepareFrame, this);
   m_sources.Subscribe();
}

DmdOverlay::~DmdOverlay()
{
   SetDropDir("");
   m_msgApi->UnsubscribeMsg(m_prepareFrameMsgId, OnPrepareFrame, this);
   m_msgApi->UnsubscribeMsg(m_overlayMsgId, OnOverlayMsg, this);
   m_sources.Unsubscribe();
   Withdraw();
   m_msgApi->ReleaseMsgID(m_prepareFrameMsgId);
   m_msgApi->ReleaseMsgID(m_overlayMsgId);
}

void DmdOverlay::SetController(uint32_t controllerEndpointId)
{
   m_controllerEndpointId = controllerEndpointId;
   m_sources.Refresh();
}

void DmdOverlay::FilterSources(std::vector<DisplaySrcId>& items)
{
   // The controller's own display, with a render frame in a format we can composite.
   // Our own override is on this endpoint and is excluded by the controller test.
   const uint32_t controller = m_controllerEndpointId;
   DisplaySrcId selected { };
   for (const DisplaySrcId& item : items)
   {
      if (controller == 0 || item.id.endpointId != controller || item.GetRenderFrame == nullptr)
         continue;
      if (item.frameFormat != CTLPI_DISPLAY_FORMAT_LUM32F && item.frameFormat != CTLPI_DISPLAY_FORMAT_SRGB888)
         continue;
      if (selected.id.id == 0 || item.width * item.height > selected.width * selected.height)
         selected = item;
   }
   items.clear();
   if (selected.id.id != 0)
      items.push_back(selected);
}

void DmdOverlay::OnSourcesChanged()
{
   const DisplaySrcId src = m_sources.With([](const std::vector<DisplaySrcId>& items) { return items.empty() ? DisplaySrcId { } : items.front(); });

   bool changed;
   {
      std::lock_guard lock(m_mutex);
      changed = !(src == m_source);
      if (changed)
         m_source = src;
   }
   if (!changed)
      return;

   Withdraw();
   if (src.id.id != 0)
      Publish();
   else if (m_controllerEndpointId != 0)
      LOGI("overlay: controller publishes no display we can composite on, overlay disabled"s);
}

void DmdOverlay::Publish()
{
   DisplaySrcId id { };
   {
      std::lock_guard lock(m_mutex);
      const size_t n = static_cast<size_t>(m_source.width) * m_source.height;
      for (auto& b : m_lum) { b.assign(n, 0.f); }
      for (auto& b : m_rgb) { b.assign(n * 3, 0); }
      m_target = 0;
      m_srcFrameId = 0;
      m_lastComposited = false;
      m_published = true;

      id.id = { { m_endpointId, 0 } };
      id.overrideId = m_source.id;
      id.callContext = this;
      id.width = m_source.width;
      id.height = m_source.height;
      id.hardware = m_source.hardware;
      id.frameFormat = m_source.frameFormat;
      id.GetRenderFrame = &DmdOverlay::GetRenderFrame;
      id.identifyFormat = m_source.GetIdentifyFrame ? m_source.identifyFormat : 0;
      id.GetIdentifyFrame = m_source.GetIdentifyFrame ? &DmdOverlay::GetIdentifyFrame : nullptr; // pass through: identification is not ours to alter
   }
   m_override.SetItem(id);
   LOGI("overlay: overriding display [endpointId="s + std::to_string(m_source.id.endpointId) + "." + std::to_string(m_source.id.resId) + ", " + std::to_string(m_source.width) + "x"
      + std::to_string(m_source.height) + (m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? ", lum32f]" : ", srgb888]"));
}

void DmdOverlay::Withdraw()
{
   bool was;
   {
      std::lock_guard lock(m_mutex);
      was = m_published;
      m_published = false;
   }
   if (was)
   {
      m_override.ClearItems();
      LOGI("overlay: override withdrawn"s);
   }
}

bool DmdOverlay::Upload(const uint8_t* payload, size_t size)
{
   if (payload == nullptr || size < 2)
   {
      LOGE("overlay: upload rejected, payload too short"s);
      return false;
   }
   const uint32_t w = payload[0], h = payload[1];
   if (size != 2 + static_cast<size_t>(w) * h)
   {
      LOGE("overlay: upload rejected, "s + std::to_string(size) + " bytes for " + std::to_string(w) + "x" + std::to_string(h));
      return false;
   }
   {
      std::lock_guard lock(m_mutex);
      if (m_published && (w > m_source.width || h > m_source.height))
      {
         LOGE("overlay: upload rejected, "s + std::to_string(w) + "x" + std::to_string(h) + " exceeds the " + std::to_string(m_source.width) + "x" + std::to_string(m_source.height) + " display");
         return false;
      }
      m_bitmap.width = w;
      m_bitmap.height = h;
      m_bitmap.pixels.assign(payload + 2, payload + size);
      m_lastComposited = false; // force a recomposite if currently visible
   }
   LOGI("overlay: uploaded "s + std::to_string(w) + "x" + std::to_string(h) + " sha256=" + Sha256Hex(payload, size));
   return true;
}

void DmdOverlay::Show(uint32_t durationMs)
{
   {
      std::lock_guard lock(m_mutex);
      if (m_bitmap.pixels.empty())
      {
         LOGW("overlay: show with no bitmap uploaded"s);
         return;
      }
      m_visible = true;
      m_untilHidden = durationMs == 0;
      m_visibleUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
      m_lastComposited = false;
      m_compositedFrames = 0;
   }
   LOGI(durationMs == 0 ? "overlay: shown until hidden"s : "overlay: shown for "s + std::to_string(durationMs) + " ms");
}

void DmdOverlay::Hide()
{
   uint64_t frames;
   {
      std::lock_guard lock(m_mutex);
      m_visible = false;
      frames = m_compositedFrames;
   }
   LOGI("overlay: hidden after compositing "s + std::to_string(frames) + " frames");
}

void DmdOverlay::OnOverlayMsg(const unsigned int msgId, void* userData, void* msgData)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(userData);
   ScorbitOverlayMsg* msg = static_cast<ScorbitOverlayMsg*>(msgData);
   if (msg == nullptr || msg->version != 1)
      return;
   switch (msg->op)
   {
   case SCORBIT_OVERLAY_OP_UPLOAD: msg->applied = me->Upload(msg->payload, msg->payloadSize) ? 1 : 0; break;
   case SCORBIT_OVERLAY_OP_SHOW: me->Show(msg->durationMs); msg->applied = 1; break;
   case SCORBIT_OVERLAY_OP_HIDE: me->Hide(); msg->applied = 1; break;
   default: msg->applied = 0; break;
   }
}

// Render path. Called by VPX (and any downstream consumer) on their threads.
// Identify frames are forwarded to the controller untouched, on its own call context.
DisplayFrame DmdOverlay::GetIdentifyFrame(void* callContext)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(callContext);
   std::lock_guard lock(me->m_mutex);
   if (!me->m_published || me->m_source.GetIdentifyFrame == nullptr)
      return { 0, nullptr };
   return me->m_source.GetIdentifyFrame(me->m_source.callContext);
}

DisplayFrame DmdOverlay::GetRenderFrame(void* callContext)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(callContext);
   if (me == nullptr)
      return { 0, nullptr };
   std::lock_guard lock(me->m_mutex);
   if (!me->m_published)
      return { me->m_outFrameId, me->m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? static_cast<const void*>(me->m_lum[0].data()) : me->m_rgb[0].data() };

   // The controller's frame. Thread safe by contract, and the only cross-plugin call in here.
   const DisplayFrame src = me->m_source.GetRenderFrame(me->m_source.callContext);
   if (src.frame == nullptr)
      return { me->m_outFrameId, me->m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? static_cast<const void*>(me->m_lum[me->m_target ^ 1].data()) : me->m_rgb[me->m_target ^ 1].data() };

   bool visible = me->m_visible && !me->m_bitmap.pixels.empty();
   if (visible && !me->m_untilHidden && std::chrono::steady_clock::now() >= me->m_visibleUntil)
   {
      me->m_visible = false; // timed out; the API thread is not involved, so no log here
      visible = false;
   }

   if (!visible)
   {
      // Pass the controller's frame through untouched.
      if (me->m_lastComposited || src.frameId != me->m_srcFrameId)
         me->m_outFrameId++;
      me->m_lastComposited = false;
      me->m_srcFrameId = src.frameId;
      return { me->m_outFrameId, src.frame };
   }

   if (!me->m_lastComposited || src.frameId != me->m_srcFrameId)
   {
      const void* out = me->Composite(src);
      me->m_lastComposited = true;
      me->m_srcFrameId = src.frameId;
      me->m_outFrameId++;
      me->m_compositedFrames++;
      return { me->m_outFrameId, out };
   }
   return { me->m_outFrameId, me->m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? static_cast<const void*>(me->m_lum[me->m_target ^ 1].data()) : me->m_rgb[me->m_target ^ 1].data() };
}

// Composite the bitmap over the controller's frame into the spare buffer, mirroring
// what the probe firmware does (spb-firmwares/RP2040/dmd.cpp, Dmd::FrameToSamples):
// the bitmap is centred on the display, pixels outside it and pixels with 0x80 set
// are transparent, and the brightness bits drive the display's bitplanes directly.
// On a 2-bitplane display the probe sums bits 0..2 (COLOR_IS_A_SUM), on a 4-bitplane
// display the low four bits are the shade. Luminance is then shade / (shades - 1).
const void* DmdOverlay::Composite(const DisplayFrame& src)
{
   const uint32_t W = m_source.width, H = m_source.height;
   const uint32_t bw = std::min(m_bitmap.width, W), bh = std::min(m_bitmap.height, H);
   const uint32_t x0 = (W - bw) / 2, y0 = (H - bh) / 2;
   const bool fourPlanes = m_source.identifyFormat == CTLPI_DISPLAY_ID_FORMAT_BITPLANE4;
   const float maxShade = fourPlanes ? 15.f : 3.f;

   auto shadeOf = [&](uint8_t px) -> float
   {
      if (fourPlanes)
         return static_cast<float>(px & 0x0F);
      return static_cast<float>((px & 1) + ((px >> 1) & 1) + ((px >> 2) & 1));
   };

   if (m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F)
   {
      std::vector<float>& out = m_lum[m_target];
      std::memcpy(out.data(), src.frame, out.size() * sizeof(float));
      for (uint32_t y = 0; y < bh; y++)
      {
         const uint8_t* row = m_bitmap.pixels.data() + static_cast<size_t>(y) * m_bitmap.width;
         float* dst = out.data() + static_cast<size_t>(y0 + y) * W + x0;
         for (uint32_t x = 0; x < bw; x++)
            if (!(row[x] & 0x80))
               dst[x] = shadeOf(row[x]) / maxShade;
      }
      m_target ^= 1;
      return out.data();
   }

   std::vector<uint8_t>& out = m_rgb[m_target];
   std::memcpy(out.data(), src.frame, out.size());
   for (uint32_t y = 0; y < bh; y++)
   {
      const uint8_t* row = m_bitmap.pixels.data() + static_cast<size_t>(y) * m_bitmap.width;
      uint8_t* dst = out.data() + (static_cast<size_t>(y0 + y) * W + x0) * 3;
      for (uint32_t x = 0; x < bw; x++)
         if (!(row[x] & 0x80))
         {
            const uint8_t g = static_cast<uint8_t>(shadeOf(row[x]) / maxShade * 255.f + 0.5f);
            dst[x * 3] = g; dst[x * 3 + 1] = g; dst[x * 3 + 2] = g;
         }
   }
   m_target ^= 1;
   return out.data();
}

// Demo file drop: the raw 0x23 / 0x27 payloads the daemon would hand to the cable,
// picked up from disk and pushed through the same Overlay message the socket
// worker will use later.
void DmdOverlay::SetDropDir(const std::string& dir)
{
   if (m_dropRunning.exchange(false) && m_dropThread.joinable())
      m_dropThread.join();
   {
      std::lock_guard lock(m_dropMutex);
      m_dropDir = dir;
   }
   if (dir.empty())
      return;
   std::error_code ec;
   std::filesystem::create_directories(dir, ec);
   m_dropRunning = true;
   m_dropThread = std::thread(&DmdOverlay::DropWorker, this);
   LOGI("overlay: watching "s + dir + " for overlay.bin / overlay-ctl.bin");
}

void DmdOverlay::DropWorker()
{
   std::filesystem::file_time_type seen[2] { };
   const char* names[2] = { "overlay.bin", "overlay-ctl.bin" };
   const int ops[2] = { SCORBIT_OVERLAY_OP_UPLOAD, 0 };
   while (m_dropRunning)
   {
      std::this_thread::sleep_for(DROP_POLL);
      std::filesystem::path dir;
      {
         std::lock_guard lock(m_dropMutex);
         dir = m_dropDir;
      }
      for (int i = 0; i < 2; i++)
      {
         std::error_code ec;
         const std::filesystem::path p = dir / names[i];
         const auto t = std::filesystem::last_write_time(p, ec);
         if (ec || t == seen[i])
            continue;
         seen[i] = t;
         std::ifstream f(p, std::ios::binary);
         std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
         if (bytes.empty())
            continue;
         DropEvent ev { ops[i], std::move(bytes) };
         if (i == 1)
            ev.op = ev.bytes[0] ? SCORBIT_OVERLAY_OP_SHOW : SCORBIT_OVERLAY_OP_HIDE;
         std::lock_guard lock(m_dropMutex);
         m_dropQueue.push_back(std::move(ev));
      }
   }
}

void DmdOverlay::OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData)
{
   static_cast<DmdOverlay*>(userData)->DrainDrops();
}

// API thread, once per frame. Each queued payload goes through the same Overlay
// message the daemon transport will use, so the demo exercises the real seam.
void DmdOverlay::DrainDrops()
{
   std::deque<DropEvent> pending;
   {
      std::lock_guard lock(m_dropMutex);
      pending.swap(m_dropQueue);
   }
   for (DropEvent& ev : pending)
   {
      ScorbitOverlayMsg msg { };
      msg.version = 1;
      msg.op = ev.op;
      if (ev.op == SCORBIT_OVERLAY_OP_UPLOAD)
      {
         msg.payload = ev.bytes.data();
         msg.payloadSize = static_cast<uint32_t>(ev.bytes.size());
      }
      else if (ev.op == SCORBIT_OVERLAY_OP_SHOW && ev.bytes.size() >= 3)
         msg.durationMs = (static_cast<uint32_t>(ev.bytes[1]) << 8) | ev.bytes[2];
      m_msgApi->BroadcastMsg(m_endpointId, m_overlayMsgId, &msg);
   }
}

}
