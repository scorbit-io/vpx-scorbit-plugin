// license:GPLv3+

#include "common.h"
#include "VpxSessionSource.h"

// The plugin API carries no runtime version, so what the plugin can honestly
// declare is the build its headers were pinned to. cmake/vpx_headers.cmake owns
// these values and CMakeLists.txt passes them in.
#ifndef SCORBIT_VPX_PINNED_VERSION
#define SCORBIT_VPX_PINNED_VERSION ""
#endif
#ifndef SCORBIT_VPX_PINNED_REVISION
#define SCORBIT_VPX_PINNED_REVISION ""
#endif

namespace Scorbit
{

// Beyond this the worker is producing log lines faster than the API thread
// drains them, which only happens if VPX has stopped ticking; keep the newest.
static constexpr size_t LOG_QUEUE_LIMIT = 256;

VpxSessionSource::VpxSessionSource(const MsgPluginAPI* msgApi, uint32_t endpointId, VPXPluginAPI* vpxApi, DmdTap& tap)
   : m_msgApi(msgApi)
   , m_endpointId(endpointId)
   , m_vpxApi(vpxApi)
   , m_tap(tap)
   , m_gameStartMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START))
   , m_gameEndMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END))
   , m_prepareFrameMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME))
{
   m_msgApi->SubscribeMsg(m_endpointId, m_gameStartMsgId, OnGameStart, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_gameEndMsgId, OnGameEnd, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_prepareFrameMsgId, OnPrepareFrame, this);
}

VpxSessionSource::~VpxSessionSource()
{
   m_msgApi->UnsubscribeMsg(m_prepareFrameMsgId, OnPrepareFrame, this);
   m_msgApi->UnsubscribeMsg(m_gameEndMsgId, OnGameEnd, this);
   m_msgApi->UnsubscribeMsg(m_gameStartMsgId, OnGameStart, this);
   m_msgApi->ReleaseMsgID(m_prepareFrameMsgId);
   m_msgApi->ReleaseMsgID(m_gameEndMsgId);
   m_msgApi->ReleaseMsgID(m_gameStartMsgId);
   DrainLog();
}

void VpxSessionSource::OnGameStart(const unsigned int, void* userData, void*)
{
   VpxSessionSource* self = static_cast<VpxSessionSource*>(userData);
   std::string tablePath;
   if (self->m_vpxApi != nullptr)
   {
      VPXTableInfo info { };
      self->m_vpxApi->GetTableInfo(&info);
      if (info.path != nullptr)
         tablePath = Wire::TableFileName(info.path);
   }

   {
      std::lock_guard lock(self->m_mutex);
      self->m_tablePath = std::move(tablePath);
      // A table restart with the same ROM and the same display changes nothing
      // on the wire, so the epoch is what makes Declare go out again anyway.
      self->m_epoch++;
   }
   // Published last, so the worker never sees a running player with no table
   // path: it reads the flag and the path in two separate steps.
   self->m_playerRunning = true;
}

void VpxSessionSource::OnGameEnd(const unsigned int, void* userData, void*)
{
   VpxSessionSource* self = static_cast<VpxSessionSource*>(userData);
   // Cleared first here, for the same reason it is set last above.
   self->m_playerRunning = false;
   std::lock_guard lock(self->m_mutex);
   self->m_tablePath.clear();
   self->m_epoch++;
}

void VpxSessionSource::OnPrepareFrame(const unsigned int, void* userData, void*)
{
   // Counts what Pong reports. The log is drained from the plugin's own timer
   // instead, because this fires only while a player is running and the
   // interesting lines happen while VPX sits at the table chooser.
   static_cast<VpxSessionSource*>(userData)->m_renderedFrame++;
}

void VpxSessionSource::SetRomId(const std::string& romId)
{
   std::lock_guard lock(m_mutex);
   m_romId = romId;
}

void VpxSessionSource::PushLog(int level, const std::string& message)
{
   std::lock_guard lock(m_logMutex);
   if (m_logQueue.size() >= LOG_QUEUE_LIMIT)
   {
      m_logQueue.pop_front();
      m_logDropped++;
   }
   m_logQueue.emplace_back(level, message);
}

void VpxSessionSource::DrainLog()
{
   std::deque<std::pair<int, std::string>> pending;
   uint64_t dropped = 0;
   {
      std::lock_guard lock(m_logMutex);
      if (m_logQueue.empty() && m_logDropped == 0)
         return;
      pending.swap(m_logQueue);
      dropped = m_logDropped;
      m_logDropped = 0;
   }

   if (dropped != 0)
      LOGW("Socket: dropped "s + std::to_string(dropped) + " log lines");

   for (const auto& [level, message] : pending)
   {
      switch (level)
      {
      case LOG_LEVEL_DEBUG: LOGD(message); break;
      case LOG_LEVEL_WARN: LOGW(message); break;
      case LOG_LEVEL_ERROR: LOGE(message); break;
      default: LOGI(message); break;
      }
   }
}

void VpxSessionSource::Refresh()
{
   // The frame carries the generation of the source that produced it, so
   // "selected, but nothing captured from this source yet" is read off the
   // frame rather than inferred. It has to be: the tap keeps the last frame of
   // the previous source after a replacement, and its frame ids may restart at
   // values the daemon has already seen, so serving that frame as if it came
   // from the new source would hand over stale geometry and stale pixels under
   // a frame id the daemon would take for one it already has.
   //
   // Reading the frame and then the generation is safe in that order. The tap
   // stamps a frame under the same lock that bumps the counter, so a frame's
   // generation can never be ahead of the counter, only behind it. Equal means
   // the frame came from the source that is selected now.
   const bool tapHasSource = m_tap.HasSource();
   const bool haveFrame = m_tap.GetLatest(m_latest);
   const uint64_t tapGeneration = m_tap.SourceGeneration();
   m_hasSource = tapHasSource && haveFrame && m_latest.generation == tapGeneration;

   // During that gap the daemon still sees the generation move, so it discards
   // its cache and stops treating the frame id it last saw as current.
   m_generation = m_hasSource ? m_latest.generation : tapGeneration;
}

void VpxSessionSource::GetDeclare(DeclareSnapshot& out)
{
   std::lock_guard lock(m_mutex);
   Refresh();

   Wire::Declare& d = out.declare;
   d.romId = m_romId;
   d.tablePath = m_tablePath;
   d.vpxVersion = SCORBIT_VPX_PINNED_VERSION;
   d.vpxRevision = SCORBIT_VPX_PINNED_REVISION;
   d.width = static_cast<uint16_t>(m_hasSource ? m_latest.width : 0);
   d.height = static_cast<uint16_t>(m_hasSource ? m_latest.height : 0);
   d.shades = static_cast<uint8_t>(m_hasSource ? m_latest.shades : 0);
   d.sourceGeneration = static_cast<uint32_t>(m_generation);
   d.identifyFormat = !m_hasSource ? "" : (m_latest.shades == 16 ? "BITPLANE4" : "BITPLANE2");
   out.epoch = m_epoch;
}

void VpxSessionSource::GetFrame(FrameSnapshot& out)
{
   std::lock_guard lock(m_mutex);
   Refresh();

   out.hasSource = m_hasSource;
   out.generation = static_cast<uint32_t>(m_generation);
   if (!m_hasSource)
   {
      out.frameId = 0;
      out.width = 0;
      out.height = 0;
      out.shades = 0;
      out.pixels.clear();
      return;
   }
   out.frameId = m_latest.frameId;
   out.width = static_cast<uint16_t>(m_latest.width);
   out.height = static_cast<uint16_t>(m_latest.height);
   out.shades = static_cast<uint8_t>(m_latest.shades);
   // Handed over exactly as the tap captured it: one shade index per pixel,
   // row major. Nothing repacks it on the way to the daemon.
   out.pixels = m_latest.pixels;
}

void VpxSessionSource::GetSession(SessionSnapshot& out)
{
   out.renderedFrame = m_renderedFrame;
   out.playerRunning = m_playerRunning;
}

}
