// license:GPLv3+

#include "common.h"
#include "DmdOverlay.h"
#include "OverlayPlacement.h"
#include "ScorbitPluginAPI.h"

#include "pinmame/PinMAMEPlugin.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace PinballPlugin::Controller;

namespace Scorbit
{

static constexpr auto DROP_POLL = std::chrono::milliseconds(100);
static constexpr double WITHDRAW_DELAY = 0.001; // seconds; any positive delay queues the call

// The plugin's one overlay, so a deferred withdrawal can tell it is still alive. API thread only.
static DmdOverlay* liveOverlay = nullptr;

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
   , m_machineStateMsgId(msgApi->GetMsgID(PMPI_NAMESPACE, PMPI_GET_MACHINE_STATE))
   , m_sources(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
        [this](std::vector<DisplaySrcId>& items) { FilterSources(items); }, nullptr, [this]() { OnSourcesChanged(); })
   , m_override(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG)
{
   liveOverlay = this;
   m_msgApi->SubscribeMsg(m_endpointId, m_overlayMsgId, OnOverlayMsg, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_prepareFrameMsgId, OnPrepareFrame, this);
   m_sources.Subscribe();
}

DmdOverlay::~DmdOverlay()
{
   liveOverlay = nullptr;
   Detach();
   Withdraw();
   m_msgApi->ReleaseMsgID(m_machineStateMsgId);
   m_msgApi->ReleaseMsgID(m_prepareFrameMsgId);
   m_msgApi->ReleaseMsgID(m_overlayMsgId);
}

void DmdOverlay::Detach()
{
   if (m_detached)
      return;
   m_detached = true;
   {
      // The override may stay listed (quit path) after the controller's source is gone;
      // serve our own buffers from here on, never its callbacks.
      std::lock_guard lock(m_mutex);
      m_published = false;
   }
   m_surface = false;
   SetDropDir("");
   m_msgApi->UnsubscribeMsg(m_prepareFrameMsgId, OnPrepareFrame, this);
   m_msgApi->UnsubscribeMsg(m_overlayMsgId, OnOverlayMsg, this);
   m_sources.Unsubscribe();
}

void DmdOverlay::SetController(uint32_t controllerEndpointId)
{
   m_controllerEndpointId = controllerEndpointId;
   m_platformKnown = false;
   {
      std::lock_guard lock(m_mutex);
      m_platform = Overlay::Platform::Unknown;
   }
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

   if (src.id.id == 0)
   {
      WithdrawDeferred();
      if (m_controllerEndpointId != 0)
         LOGI("overlay: controller publishes no display we can composite on, overlay disabled"s);
      return;
   }
   Withdraw();
   Publish();
}

void DmdOverlay::Publish()
{
   DisplaySrcId id { };
   {
      std::lock_guard lock(m_mutex);
      const size_t n = static_cast<size_t>(m_source.width) * m_source.height;
      for (auto& b : m_lum) { b.assign(n, 0.f); }
      for (auto& b : m_rgb) { b.assign(n * 3, 0); }
      m_idBlank.assign(n, 0);
      m_lastIdFrameId = 0;
      m_pubFrameFormat = m_source.frameFormat;
      m_pubIdentify = m_source.GetIdentifyFrame != nullptr;
      m_target = 0;
      m_srcFrameId = 0;
      m_lastComposited = false;
      m_published = true;
      m_withdrawPending = false;

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
   m_surface = true;
   LOGI("overlay: overriding display [endpointId="s + std::to_string(m_source.id.endpointId) + "." + std::to_string(m_source.id.resId) + ", " + std::to_string(m_source.width) + "x"
      + std::to_string(m_source.height) + (m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? ", lum32f]" : ", srgb888]"));
}

void DmdOverlay::Withdraw()
{
   {
      std::lock_guard lock(m_mutex);
      m_published = false;
   }
   // m_surface is left alone: a swap publishes again at once, and the worker must not
   // see the gap. Every path that ends with no override clears it itself.
   m_withdrawPending = false;
   if (!m_override.GetItems().empty())
   {
      m_override.ClearItems();
      LOGI("overlay: override withdrawn"s);
   }
}

void DmdOverlay::WithdrawDeferred()
{
   bool was;
   {
      // GetRenderFrame serves our own buffer from here on and never calls the controller.
      std::lock_guard lock(m_mutex);
      was = m_published;
      m_published = false;
   }
   m_surface = false;
   if (was && !m_withdrawPending)
   {
      m_withdrawPending = true;
      // Not 0: on the API thread a delay <= 0 runs the callback right here (VPX
      // MsgPluginManager::RunOnMainThread), still inside the teardown broadcast.
      m_msgApi->RunOnMainThread(m_endpointId, WITHDRAW_DELAY, OnDeferredWithdraw, this);
   }
}

void MSGPIAPI DmdOverlay::OnDeferredWithdraw(void* userData)
{
   // The plugin's single instance; a callback that outlives it finds it gone.
   DmdOverlay* me = static_cast<DmdOverlay*>(userData);
   if (me != liveOverlay || !me->m_withdrawPending || me->m_detached)
      return;
   me->m_withdrawPending = false;
   bool published;
   {
      std::lock_guard lock(me->m_mutex);
      published = me->m_published;
   }
   if (!published && !me->m_override.GetItems().empty())
   {
      me->m_override.ClearItems();
      LOGI("overlay: override withdrawn"s);
   }
}

Overlay::State::Upload DmdOverlay::Store(const uint8_t* payload, size_t size)
{
   std::lock_guard lock(m_mutex);
   const Overlay::State::Upload result = m_state.Store(payload, size);
   if (result == Overlay::State::Upload::Stored)
      m_lastComposited = false; // replaces a visible bitmap from the next frame
   return result;
}

void DmdOverlay::Control(bool enable, uint32_t durationMs)
{
   std::lock_guard lock(m_mutex);
   m_state.Control(enable, durationMs, std::chrono::steady_clock::now());
   m_lastComposited = false;
   if (enable)
      m_compositedFrames = 0;
}

void DmdOverlay::Clear()
{
   std::lock_guard lock(m_mutex);
   m_state.Clear();
   m_lastComposited = false;
}

// API thread. applied means accepted, decided from the payload and the surface alone.
void DmdOverlay::OnOverlayMsg(const unsigned int msgId, void* userData, void* msgData)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(userData);
   ScorbitOverlayMsg* msg = static_cast<ScorbitOverlayMsg*>(msgData);
   if (msg == nullptr || msg->version != 1)
      return;
   msg->applied = 0;
   if (!me->HasSurface())
   {
      LOGW("overlay: refused, the controller publishes no display to composite on"s);
      return;
   }
   switch (msg->op)
   {
   case SCORBIT_OVERLAY_OP_UPLOAD:
      switch (me->Store(msg->payload, msg->payloadSize))
      {
      case Overlay::State::Upload::Malformed:
         LOGE("overlay: upload rejected, "s + std::to_string(msg->payloadSize) + " bytes is shorter than its width and height say");
         break;
      case Overlay::State::Upload::Ignored:
         // As the probe firmware does: ignored, and the previous overlay kept.
         LOGW("overlay: upload ignored, "s + std::to_string(msg->payload[0]) + "x" + std::to_string(msg->payload[1]) + " is outside "
            + std::to_string(Overlay::MIN_SIDE) + "x" + std::to_string(Overlay::MIN_SIDE) + " to "
            + std::to_string(Overlay::MAX_WIDTH) + "x" + std::to_string(Overlay::MAX_HEIGHT) + "; previous overlay kept");
         break;
      case Overlay::State::Upload::Stored:
      {
         const size_t used = 2 + static_cast<size_t>(msg->payload[0]) * msg->payload[1];
         LOGI("overlay: uploaded "s + std::to_string(msg->payload[0]) + "x" + std::to_string(msg->payload[1]) + " sha256=" + Sha256Hex(msg->payload, used));
         msg->applied = 1;
         break;
      }
      }
      break;
   case SCORBIT_OVERLAY_OP_SHOW:
      // Arms even before any upload, as the probe does: an upload inside the window shows at once.
      me->Control(true, msg->durationMs);
      LOGI(msg->durationMs == 0 ? "overlay: shown until hidden"s : "overlay: shown for "s + std::to_string(msg->durationMs) + " ms");
      msg->applied = 1;
      break;
   case SCORBIT_OVERLAY_OP_HIDE:
   {
      uint64_t frames;
      {
         std::lock_guard lock(me->m_mutex);
         frames = me->m_compositedFrames;
      }
      me->Control(false, 0);
      LOGI("overlay: hidden after compositing "s + std::to_string(frames) + " frames");
      msg->applied = 1;
      break;
   }
   default: break;
   }
}

// Render path. Called by VPX (and any downstream consumer) on their threads.
// Identify frames are forwarded to the controller untouched, on its own call context.
DisplayFrame DmdOverlay::GetIdentifyFrame(void* callContext)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(callContext);
   std::lock_guard lock(me->m_mutex);
   if (!me->m_published)
   {
      if (!me->m_pubIdentify || me->m_idBlank.empty())
         return { 0, nullptr };
      return { me->m_lastIdFrameId, me->m_idBlank.data() };
   }
   if (me->m_source.GetIdentifyFrame == nullptr)
      return { 0, nullptr };
   const DisplayFrame frame = me->m_source.GetIdentifyFrame(me->m_source.callContext);
   me->m_lastIdFrameId = frame.frameId;
   return frame;
}

DisplayFrame DmdOverlay::GetRenderFrame(void* callContext)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(callContext);
   if (me == nullptr)
      return { 0, nullptr };
   std::lock_guard lock(me->m_mutex);
   if (!me->m_published)
      return { me->m_outFrameId, me->m_pubFrameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? static_cast<const void*>(me->m_lum[0].data()) : me->m_rgb[0].data() };

   // The controller's frame. Thread safe by contract, and the only cross-plugin call in here.
   const DisplayFrame src = me->m_source.GetRenderFrame(me->m_source.callContext);
   if (src.frame == nullptr)
      return { me->m_outFrameId, me->m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F ? static_cast<const void*>(me->m_lum[me->m_target ^ 1].data()) : me->m_rgb[me->m_target ^ 1].data() };

   // Judged on every call: the timer expires here, with no writer involved.
   if (!me->m_state.Visible(std::chrono::steady_clock::now()))
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
// the bitmap is centred on the display, pixels outside it and pixels with bit 7 set
// are transparent, and an opaque pixel replaces the dot at the luminance the probe on
// this platform shows its value at.
const void* DmdOverlay::Composite(const DisplayFrame& src)
{
   const uint32_t W = m_source.width;
   const Overlay::Placement at = Overlay::Place(W, m_source.height, m_state.Width(), m_state.Height());
   const uint32_t bw = at.width, bh = at.height, x0 = at.dstX, y0 = at.dstY;
   const bool sixteenShades = m_source.identifyFormat == CTLPI_DISPLAY_ID_FORMAT_BITPLANE4;
   const std::vector<uint8_t>& pixels = m_state.Pixels();
   const Overlay::Platform platform = m_platform;

   if (m_source.frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F)
   {
      std::vector<float>& out = m_lum[m_target];
      std::memcpy(out.data(), src.frame, out.size() * sizeof(float));
      for (uint32_t y = 0; y < bh; y++)
      {
         const uint8_t* row = pixels.data() + static_cast<size_t>(at.srcY + y) * m_state.Width() + at.srcX;
         float* dst = out.data() + static_cast<size_t>(y0 + y) * W + x0;
         for (uint32_t x = 0; x < bw; x++)
            if (!(row[x] & Overlay::TRANSPARENT_BIT))
               dst[x] = Overlay::Luminance(platform, row[x], sixteenShades);
      }
      m_target ^= 1;
      return out.data();
   }

   std::vector<uint8_t>& out = m_rgb[m_target];
   std::memcpy(out.data(), src.frame, out.size());
   for (uint32_t y = 0; y < bh; y++)
   {
      const uint8_t* row = pixels.data() + static_cast<size_t>(at.srcY + y) * m_state.Width() + at.srcX;
      uint8_t* dst = out.data() + (static_cast<size_t>(y0 + y) * W + x0) * 3;
      for (uint32_t x = 0; x < bw; x++)
         if (!(row[x] & Overlay::TRANSPARENT_BIT))
         {
            const uint8_t g = static_cast<uint8_t>(Overlay::Luminance(platform, row[x], sixteenShades) * 255.f + 0.5f);
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
         if (i == 1) // control bit 0 only; the others are reserved
            ev.op = (ev.bytes[0] & 1) ? SCORBIT_OVERLAY_OP_SHOW : SCORBIT_OVERLAY_OP_HIDE;
         std::lock_guard lock(m_dropMutex);
         m_dropQueue.push_back(std::move(ev));
      }
   }
}

void DmdOverlay::OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData)
{
   DmdOverlay* me = static_cast<DmdOverlay*>(userData);
   me->QueryPlatform();
   me->DrainDrops();
}

void DmdOverlay::QueryPlatform()
{
   if (m_platformKnown || m_controllerEndpointId == 0)
      return;
   PinMAMEMachineStateMsg state { };
   state.version = 1;
   m_msgApi->BroadcastMsg(m_endpointId, m_machineStateMsgId, &state);
   if (state.hardwareGen == 0)
      return;
   m_platformKnown = true;
   const Overlay::Platform platform = Overlay::PlatformOf(state.hardwareGen);
   {
      std::lock_guard lock(m_mutex);
      m_platform = platform;
      m_lastComposited = false;
   }
   char gen[32];
   std::snprintf(gen, sizeof(gen), "0x%llx", static_cast<unsigned long long>(state.hardwareGen));
   LOGI("overlay: shading as "s + Overlay::PlatformName(platform) + " (hardwareGen " + gen + ")");
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
