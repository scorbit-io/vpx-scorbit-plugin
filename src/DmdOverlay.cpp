// license:GPLv3+

#include "common.h"
#include "DmdOverlay.h"
#include "OverlayPlacement.h"
#include "OverrideChain.h"
#include "ScorbitPluginAPI.h"

#include "pinmame/PinMAMEPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace PinballPlugin::Controller;

namespace Scorbit
{

static constexpr auto DROP_POLL = std::chrono::milliseconds(100);
static constexpr double DEFER_DELAY = 0.001; // seconds; any positive delay queues the call
// Consumers drop a withdrawn entry inside our broadcast, by the CtrlItemConsumer contract. The
// margin is for any that does not; nothing has to wait for it.
static constexpr auto RETIRE_AFTER = std::chrono::seconds(1);

// The plugin's one overlay, so a deferred apply can tell it is still alive. API thread only.
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

// Formats an override can composite into, and their size per dot.
size_t BytesPerPixel(unsigned int format)
{
   switch (format)
   {
   case CTLPI_DISPLAY_FORMAT_LUM32F: return sizeof(float);
   case CTLPI_DISPLAY_FORMAT_SRGB888: return 3;
   case CTLPI_DISPLAY_FORMAT_SRGB565: return 2;
   default: return 0;
   }
}

const char* FormatName(unsigned int format)
{
   switch (format)
   {
   case CTLPI_DISPLAY_FORMAT_LUM32F: return "lum32f";
   case CTLPI_DISPLAY_FORMAT_SRGB888: return "srgb888";
   case CTLPI_DISPLAY_FORMAT_SRGB565: return "srgb565";
   default: return "?";
   }
}

// LUM32F is linear; an sRGB output needs it encoded.
float LinearToSrgb(float v)
{
   return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

std::string Describe(const DisplaySrcId& s)
{
   return "[endpointId="s + std::to_string(s.id.endpointId) + "." + std::to_string(s.id.resId) + ", " + std::to_string(s.width) + "x"
      + std::to_string(s.height) + ", " + FormatName(s.frameFormat) + "]";
}

}

DmdOverlay::DmdOverlay(const MsgPluginAPI* msgApi, uint32_t endpointId)
   : m_msgApi(msgApi)
   , m_endpointId(endpointId)
   , m_overlayMsgId(msgApi->GetMsgID(SCORBIT_NAMESPACE, SCORBIT_MSG_OVERLAY))
   , m_prepareFrameMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME))
   , m_machineStateMsgId(msgApi->GetMsgID(PMPI_NAMESPACE, PMPI_GET_MACHINE_STATE))
   , m_getDisplaysMsgId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_GET_SRC_MSG))
   , m_displaysChangedMsgId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_ON_SRC_CHG_MSG))
   , m_sources(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG,
        [this](std::vector<DisplaySrcId>& items) { FilterSources(items); }, nullptr, [this]() { OnSourcesChanged(); })
{
   liveOverlay = this;
   // First, and for good: subscribers answer in subscription order, so every source that
   // subscribes later (libpinmame at game start, a colouriser at its first output) lists after us.
   m_msgApi->SubscribeMsg(m_endpointId, m_getDisplaysMsgId, OnGetDisplays, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_overlayMsgId, OnOverlayMsg, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_prepareFrameMsgId, OnPrepareFrame, this);
   m_sources.Subscribe();
}

DmdOverlay::~DmdOverlay()
{
   liveOverlay = nullptr;
   Detach();
   if (!m_entries.empty())
   {
      for (std::unique_ptr<Entry>& e : m_entries)
         m_retired.push_back({ std::move(e), std::chrono::steady_clock::now() });
      m_entries.clear();
      Broadcast();
      LOGI("overlay: overrides withdrawn"s);
   }
   m_msgApi->UnsubscribeMsg(m_getDisplaysMsgId, OnGetDisplays, this);
   m_retired.clear();
   m_msgApi->ReleaseMsgID(m_displaysChangedMsgId);
   m_msgApi->ReleaseMsgID(m_getDisplaysMsgId);
   m_msgApi->ReleaseMsgID(m_machineStateMsgId);
   m_msgApi->ReleaseMsgID(m_prepareFrameMsgId);
   m_msgApi->ReleaseMsgID(m_overlayMsgId);
}

void DmdOverlay::Detach()
{
   if (m_detached)
      return;
   m_detached = true;
   // The overrides may stay listed (quit path) after their targets are gone; serve our
   // own buffers from here on, never their callbacks.
   for (std::unique_ptr<Entry>& e : m_entries)
   {
      std::lock_guard lock(e->mutex);
      e->live = false;
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
   m_platform = static_cast<int>(Overlay::Platform::Unknown);
   if (!m_detached && NeedsApply())
      ScheduleApply();
}

void DmdOverlay::OnGetDisplays(const unsigned int, void* userData, void* msgData)
{
   const DmdOverlay* me = static_cast<const DmdOverlay*>(userData);
   GetDisplaySrcMsg* msg = static_cast<GetDisplaySrcMsg*>(msgData);
   for (const std::unique_ptr<Entry>& e : me->m_entries)
   {
      if (msg->count < msg->maxEntryCount)
         msg->entries[msg->count] = e->listed;
      msg->count++;
   }
}

void DmdOverlay::FilterSources(std::vector<DisplaySrcId>& items)
{
   std::erase_if(items, [this](const DisplaySrcId& s) { return s.id.endpointId == m_endpointId; });
}

void DmdOverlay::OnSourcesChanged()
{
   DarkenStale();
   if (!m_detached && NeedsApply())
      ScheduleApply();
}

void DmdOverlay::DarkenStale()
{
   const std::vector<DisplaySrcId> all = m_sources.With([](const std::vector<DisplaySrcId>& items) { return items; });
   for (std::unique_ptr<Entry>& e : m_entries)
   {
      if (std::find(all.begin(), all.end(), e->target.source) != all.end())
         continue;
      // Taking the lock waits out a render call in flight, so the target is never called after this.
      std::lock_guard lock(e->mutex);
      e->live = false;
   }
   UpdateSurface();
}

std::vector<DmdOverlay::Target> DmdOverlay::Desired() const
{
   const uint32_t controller = m_controllerEndpointId;
   if (controller == 0 || m_detached)
      return { };
   const std::vector<DisplaySrcId> all = m_sources.With([](const std::vector<DisplaySrcId>& items) { return items; });

   // The root: the controller's own display, with a render frame we can composite.
   const DisplaySrcId* root = nullptr;
   for (const DisplaySrcId& s : all)
   {
      if (s.id.endpointId != controller || s.GetRenderFrame == nullptr)
         continue;
      if (s.frameFormat != CTLPI_DISPLAY_FORMAT_LUM32F && s.frameFormat != CTLPI_DISPLAY_FORMAT_SRGB888)
         continue;
      if (root == nullptr || s.width * s.height > root->width * root->height)
         root = &s;
   }
   if (root == nullptr || root->width == 0 || root->height == 0)
      return { };

   std::vector<Chain::Source> chain;
   for (const DisplaySrcId& s : all)
      chain.push_back({ s.id.id, s.overrideId.id, false });
   for (const std::unique_ptr<Entry>& e : m_entries)
      chain.push_back({ e->listed.id.id, e->target.source.id.id, true });

   std::vector<Target> out;
   for (const uint64_t id : Chain::Targets(chain, root->id.id))
   {
      const auto node = std::find_if(all.begin(), all.end(), [id](const DisplaySrcId& s) { return s.id.id == id; });
      if (node->GetRenderFrame == nullptr || BytesPerPixel(node->frameFormat) == 0 || node->width == 0 || node->height == 0)
         continue;
      Target t;
      t.source = *node;
      t.identify = &*node == root && node->GetIdentifyFrame != nullptr;
      t.baseWidth = root->width;
      t.baseHeight = root->height;
      t.sixteenShades = root->identifyFormat == CTLPI_DISPLAY_ID_FORMAT_BITPLANE4;
      out.push_back(t);
   }
   return out;
}

bool DmdOverlay::NeedsApply() const
{
   const std::vector<Target> desired = Desired();
   if (desired.size() != m_entries.size())
      return true;
   for (const std::unique_ptr<Entry>& e : m_entries)
   {
      {
         std::lock_guard lock(e->mutex);
         if (!e->live)
            return true;
      }
      if (std::find(desired.begin(), desired.end(), e->target) == desired.end())
         return true;
   }
   return false;
}

void DmdOverlay::ScheduleApply()
{
   if (m_applyPending)
      return;
   m_applyPending = true;
   // Not 0: on the API thread a delay <= 0 runs the callback right here (VPX
   // MsgPluginManager::RunOnMainThread), still inside the broadcast that called us.
   m_msgApi->RunOnMainThread(m_endpointId, DEFER_DELAY, OnDeferredApply, this);
}

void MSGPIAPI DmdOverlay::OnDeferredApply(void* userData)
{
   // The plugin's single instance; a callback that outlives it finds it gone.
   DmdOverlay* me = static_cast<DmdOverlay*>(userData);
   if (me != liveOverlay || !me->m_applyPending || me->m_detached)
      return;
   me->m_applyPending = false;
   me->Apply();
}

void DmdOverlay::Apply()
{
   // Withdrawn in a broadcast every consumer has long since finished handling.
   const auto now = std::chrono::steady_clock::now();
   std::erase_if(m_retired, [now](const Retired& r) { return now - r.at >= RETIRE_AFTER; });
   const std::vector<Target> desired = Desired();

   // Two broadcasts, as CtrlItemProvider does: consumers drop what goes before anything
   // new is listed, so no one reads an entry while it is discarded.
   bool removed = false;
   for (auto it = m_entries.begin(); it != m_entries.end();)
   {
      bool live;
      {
         std::lock_guard lock((*it)->mutex);
         live = (*it)->live;
      }
      if (live && std::find(desired.begin(), desired.end(), (*it)->target) != desired.end())
      {
         ++it;
         continue;
      }
      LOGI("overlay: no longer overriding display "s + Describe((*it)->target.source));
      {
         std::lock_guard lock((*it)->mutex);
         (*it)->live = false;
      }
      m_retired.push_back({ std::move(*it), now });
      it = m_entries.erase(it);
      removed = true;
   }
   if (removed)
      Broadcast();

   // Asked again: a consumer can withdraw a source inside that broadcast, such as a colouriser
   // whose input was the override just withdrawn, and a new entry must never target it.
   bool added = false;
   for (const Target& t : removed ? Desired() : desired)
   {
      const bool have = std::any_of(m_entries.begin(), m_entries.end(), [&t](const std::unique_ptr<Entry>& e) { return e->target == t; });
      if (have)
         continue;
      m_entries.push_back(MakeEntry(t));
      LOGI("overlay: overriding display "s + Describe(t.source) + (t.source.width == t.baseWidth && t.source.height == t.baseHeight ? ""s : ", scaled from " + std::to_string(t.baseWidth) + "x" + std::to_string(t.baseHeight)));
      added = true;
   }
   if (added)
      Broadcast();
   if (removed || added)
      m_surfaceGeneration++;
   UpdateSurface();
   if (m_entries.empty() && removed && m_controllerEndpointId != 0)
      LOGI("overlay: controller publishes no display we can composite on, overlay disabled"s);
}

void DmdOverlay::UpdateSurface()
{
   bool any = false;
   for (const std::unique_ptr<Entry>& e : m_entries)
   {
      std::lock_guard lock(e->mutex);
      any = any || e->live;
   }
   if (m_surface.exchange(any) != any)
      m_surfaceGeneration++;
}

void DmdOverlay::Broadcast()
{
   m_msgApi->BroadcastMsg(m_endpointId, m_displaysChangedMsgId, nullptr);
}

std::unique_ptr<DmdOverlay::Entry> DmdOverlay::MakeEntry(const Target& t)
{
   auto e = std::make_unique<Entry>();
   e->target = t;
   e->owner = this;
   e->bytesPerPixel = BytesPerPixel(t.source.frameFormat);
   const size_t n = static_cast<size_t>(t.source.width) * t.source.height;
   for (std::vector<uint8_t>& b : e->out)
      b.assign(n * e->bytesPerPixel, 0);
   e->dark.assign(n * e->bytesPerPixel, 0);
   if (t.identify)
      e->idBlank.assign(n, 0);

   DisplaySrcId& id = e->listed;
   id.id.endpointId = m_endpointId;
   id.id.resId = m_nextResId++;
   if (m_nextResId == 0xFFFF)
      m_nextResId = 1;
   id.overrideId = t.source.id;
   id.callContext = e.get();
   id.width = t.source.width;
   id.height = t.source.height;
   id.hardware = t.source.hardware;
   id.frameFormat = t.source.frameFormat;
   id.GetRenderFrame = &DmdOverlay::GetRenderFrame;
   // Passed through on the controller's display only. A colouriser's output never exposes
   // one: the colouriser could pick ours as its input and close an override cycle.
   id.identifyFormat = t.identify ? t.source.identifyFormat : 0;
   id.GetIdentifyFrame = t.identify ? &DmdOverlay::GetIdentifyFrame : nullptr;
   return e;
}

void DmdOverlay::Control(bool enable, uint32_t durationMs)
{
   m_cache.Control(enable, durationMs);
   if (enable)
      m_compositedFrames = 0;
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
      switch (Overlay::State::Check(msg->payload, msg->payloadSize))
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
         if (!me->m_cache.PostUpload(msg->payload, msg->payloadSize))
         {
            LOGW("overlay: upload refused, "s + std::to_string(Overlay::Cache::PENDING_LIMIT) + " ops already wait for the next frame");
            break;
         }
         const size_t used = 2 + static_cast<size_t>(msg->payload[0]) * msg->payload[1];
         LOGI("overlay: uploaded "s + std::to_string(msg->payload[0]) + "x" + std::to_string(msg->payload[1]) + " sha256=" + Sha256Hex(msg->payload, used));
         msg->applied = 1;
         break;
      }
      }
      break;
   case SCORBIT_OVERLAY_OP_SHOW:
      // Arms even before any upload, as the probe does: an upload inside the window shows at once.
      if (!me->m_cache.PostControl(true, msg->durationMs))
         break;
      me->m_compositedFrames = 0;
      LOGI(msg->durationMs == 0 ? "overlay: shown until hidden"s : "overlay: shown for "s + std::to_string(msg->durationMs) + " ms");
      msg->applied = 1;
      break;
   case SCORBIT_OVERLAY_OP_HIDE:
      if (!me->m_cache.PostControl(false, 0))
         break;
      LOGI("overlay: hidden after compositing "s + std::to_string(me->m_compositedFrames.load()) + " frames");
      msg->applied = 1;
      break;
   default: break;
   }
}

// Render path. Called by VPX and every downstream consumer, on their threads, several at
// once. Touches only its own entry, under its own lock, and the overlay through the cache's
// seqlock; no writer of the overlay ever holds anything it waits on.
DisplayFrame DmdOverlay::GetIdentifyFrame(void* callContext)
{
   Entry* e = static_cast<Entry*>(callContext);
   std::lock_guard lock(e->mutex);
   if (!e->live)
      return { e->lastIdFrameId, e->idBlank.data() };
   const DisplayFrame frame = e->target.source.GetIdentifyFrame(e->target.source.callContext);
   e->lastIdFrameId = frame.frameId;
   return frame;
}

DisplayFrame DmdOverlay::GetRenderFrame(void* callContext)
{
   Entry* e = static_cast<Entry*>(callContext);
   if (e == nullptr)
      return { 0, nullptr };
   std::lock_guard lock(e->mutex);
   if (!e->live)
      return { e->outFrameId, e->dark.data() };

   // The target's frame. Thread safe by contract, and the only cross-plugin call in here.
   const DisplayFrame src = e->target.source.GetRenderFrame(e->target.source.callContext);
   if (src.frame == nullptr)
      return { e->outFrameId, e->out[e->next ^ 1].data() };

   DmdOverlay* const owner = e->owner;
   if (owner->m_cache.ReadIfChanged(e->recordSequence, e->record, e->scratch))
      e->lastComposited = false;
   const int platform = owner->m_platform;
   if (platform != e->platform)
   {
      e->platform = platform;
      e->lastComposited = false;
   }

   // Judged on every call: the timer expires here, with no writer involved.
   if (!e->record.Visible(std::chrono::steady_clock::now()))
   {
      // Pass the target's frame through untouched.
      if (e->lastComposited || src.frameId != e->srcFrameId)
         e->outFrameId++;
      e->lastComposited = false;
      e->srcFrameId = src.frameId;
      return { e->outFrameId, src.frame };
   }

   if (!e->lastComposited || src.frameId != e->srcFrameId)
   {
      const void* out = owner->Composite(*e, src);
      e->lastComposited = true;
      e->srcFrameId = src.frameId;
      e->outFrameId++;
      owner->m_compositedFrames++;
      return { e->outFrameId, out };
   }
   return { e->outFrameId, e->out[e->next ^ 1].data() };
}

// Composite the bitmap over the target's frame into the spare buffer, mirroring what the
// probe firmware does (spb-firmwares/RP2040/dmd.cpp, Dmd::FrameToSamples): the bitmap is
// placed on the controller's display, centred and cropped as the probe places it, pixels
// outside it and pixels with bit 7 set are transparent, and an opaque pixel replaces the
// dot at the luminance the probe on this platform shows its value at. A colouriser output
// of another size takes the same placement scaled to it, and shows it white x luminance.
const void* DmdOverlay::Composite(Entry& e, const DisplayFrame& src)
{
   const Overlay::Record& rec = e.record;
   const Target& t = e.target;
   const uint32_t W = t.source.width, H = t.source.height;
   std::vector<uint8_t>& out = e.out[e.next];
   std::memcpy(out.data(), src.frame, out.size());
   e.next ^= 1;

   const Overlay::Placement at = Overlay::Place(t.baseWidth, t.baseHeight, rec.width, rec.height);
   if (at.width == 0 || at.height == 0)
      return out.data();

   float lum[128];
   const Overlay::Platform platform = static_cast<Overlay::Platform>(e.platform);
   for (int v = 0; v < 128; v++)
      lum[v] = Overlay::Luminance(platform, static_cast<uint8_t>(v), t.sixteenShades);

   const unsigned int format = t.source.frameFormat;
   uint8_t srgb8[128];
   uint16_t srgb565[128];
   if (format != CTLPI_DISPLAY_FORMAT_LUM32F)
      for (int v = 0; v < 128; v++)
      {
         const float s = std::clamp(LinearToSrgb(lum[v]), 0.f, 1.f);
         srgb8[v] = static_cast<uint8_t>(s * 255.f + 0.5f);
         const uint16_t r = static_cast<uint16_t>(s * 31.f + 0.5f), g = static_cast<uint16_t>(s * 63.f + 0.5f);
         srgb565[v] = static_cast<uint16_t>((r << 11) | (g << 5) | r);
      }

   // Every output dot maps to the base dot it covers; on the controller's display that is itself.
   for (uint32_t y = 0; y < H; y++)
   {
      const uint32_t by = static_cast<uint32_t>(static_cast<uint64_t>(y) * t.baseHeight / H);
      if (by < at.dstY || by >= at.dstY + at.height)
         continue;
      const uint8_t* row = rec.pixels + static_cast<size_t>(at.srcY + (by - at.dstY)) * rec.width + at.srcX;
      for (uint32_t x = 0; x < W; x++)
      {
         const uint32_t bx = static_cast<uint32_t>(static_cast<uint64_t>(x) * t.baseWidth / W);
         if (bx < at.dstX || bx >= at.dstX + at.width)
            continue;
         const uint8_t px = row[bx - at.dstX];
         if (px & Overlay::TRANSPARENT_BIT)
            continue;
         const size_t i = static_cast<size_t>(y) * W + x;
         switch (format)
         {
         case CTLPI_DISPLAY_FORMAT_LUM32F: std::memcpy(out.data() + i * sizeof(float), &lum[px], sizeof(float)); break;
         case CTLPI_DISPLAY_FORMAT_SRGB888: out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = srgb8[px]; break;
         default: std::memcpy(out.data() + i * 2, &srgb565[px], 2); break;
         }
      }
   }
   return out.data();
}

// Demo file drop: the raw 0x23 / 0x27 payloads the daemon would hand to the cable,
// picked up from disk and pushed through the same Overlay message other plugins use.
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
   // Ops queued last frame go before anything new.
   me->m_cache.DrainPending();
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
   m_platform = static_cast<int>(platform);
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
