// license:GPLv3+

#include "common.h"
#include "VpxSessionSource.h"

#include "pinmame/PinMAMEPlugin.h"

// The plugin API carries no runtime version, so what the plugin can honestly
// declare is the build its headers were pinned to. cmake/vpx_headers.cmake owns
// these values and CMakeLists.txt passes them in.
#ifndef SCORBIT_VPX_PINNED_VERSION
#define SCORBIT_VPX_PINNED_VERSION ""
#endif
#ifndef SCORBIT_VPX_PINNED_REVISION
#define SCORBIT_VPX_PINNED_REVISION ""
#endif

// PMPI_WRITE_MEMORY is pinmame #634. A libpinmame older than that has no subscriber, so a
// broadcast leaves `applied` as it was sent, which is how the bridge tells "no handler" from
// "applied nothing".

namespace Scorbit
{

// Beyond this the worker is producing log lines faster than the API thread
// drains them, which only happens if VPX has stopped ticking; keep the newest.
static constexpr size_t LOG_QUEUE_LIMIT = 256;
static constexpr int NOTICE_MS = 5000;

VpxSessionSource::VpxSessionSource(const MsgPluginAPI* msgApi, uint32_t endpointId, VPXPluginAPI* vpxApi, DmdTap& tap, DmdOverlay& overlay,
   int (*showStatus)())
   : m_msgApi(msgApi)
   , m_endpointId(endpointId)
   , m_vpxApi(vpxApi)
   , m_tap(tap)
   , m_overlay(overlay)
   , m_showStatus(showStatus)
   , m_gameStartMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START))
   , m_gameEndMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END))
   , m_prepareFrameMsgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME))
   , m_readMemoryMsgId(msgApi->GetMsgID(PMPI_NAMESPACE, PMPI_READ_MEMORY))
   , m_writeMemoryMsgId(msgApi->GetMsgID(PMPI_NAMESPACE, PMPI_WRITE_MEMORY))
   , m_machineStateMsgId(msgApi->GetMsgID(PMPI_NAMESPACE, PMPI_GET_MACHINE_STATE))
   , m_notifier(std::chrono::steady_clock::now())
{
   m_msgApi->SubscribeMsg(m_endpointId, m_gameStartMsgId, OnGameStart, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_gameEndMsgId, OnGameEnd, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_prepareFrameMsgId, OnPrepareFrame, this);
   // So the worker's first Hello and Declare have something to send.
   UpdateDeclare();
}

VpxSessionSource::~VpxSessionSource()
{
   m_msgApi->UnsubscribeMsg(m_prepareFrameMsgId, OnPrepareFrame, this);
   m_msgApi->UnsubscribeMsg(m_gameEndMsgId, OnGameEnd, this);
   m_msgApi->UnsubscribeMsg(m_gameStartMsgId, OnGameStart, this);
   m_msgApi->ReleaseMsgID(m_machineStateMsgId);
   m_msgApi->ReleaseMsgID(m_writeMemoryMsgId);
   m_msgApi->ReleaseMsgID(m_readMemoryMsgId);
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
      // Every table, declared or not and with or without PinMAME: the daemon finds the
      // script from these (SB-5990). Never logged.
      self->m_authored.tablePath = NativePathToUtf8(info.path);
      VPXInfo vpx { };
      self->m_vpxApi->GetVpxInfo(&vpx);
      self->m_authored.vpxPath = NativePathToUtf8(vpx.path);
      self->m_authored.prefPath = NativePathToUtf8(vpx.prefPath);
   }

   self->m_tablePath = std::move(tablePath);
   // A table restart with the same ROM and the same display changes nothing
   // on the wire, so the epoch is what makes Declare go out again anyway.
   self->m_epoch++;
   self->NewEpoch();
   // Restored from the ROM: a restart with the same ROM sends no SetRomId to
   // undo the OnGameEnd that cleared it.
   self->m_gameRunning = !self->m_romId.empty();
   self->m_memory.SetGameRunning(self->m_gameRunning);
   // Each player has its own notifications, numbered from 1, so the last table's handle
   // could name someone else's here; and this table has not seen the state yet.
   self->m_noticeHandle = 0;
   self->m_notifier.NewTable();
   self->UpdateDeclare();
   self->m_playerRunning = true;
}

void VpxSessionSource::OnGameEnd(const unsigned int, void* userData, void*)
{
   VpxSessionSource* self = static_cast<VpxSessionSource*>(userData);
   // Cleared first here, for the same reason it is set last above.
   self->m_playerRunning = false;
   // The per-frame Service stops with the player, so say so here.
   self->m_gameRunning = false;
   self->m_memory.SetGameRunning(false);
   self->m_addressSpaceKnown = false;
   self->m_memory.SetAddressSpace(0);
   self->m_tablePath.clear();
   self->m_authored = { };
   self->m_ops.clear();
   self->m_loggedRomWins = false;
   self->m_loggedDroppedOps = false;
   self->m_loggedIgnoredCall = false;
   self->m_loggedQueueDrop = false;
   self->m_epoch++;
   self->NewEpoch();
   self->UpdateDeclare();
}

void VpxSessionSource::OnPrepareFrame(const unsigned int, void* userData, void*)
{
   // Counts what Pong reports, and services the memory reads, since this is the
   // API thread once per rendered frame. The log is drained from the plugin's own
   // timer instead, because this fires only while a player is running and the
   // interesting lines happen while VPX sits at the table chooser.
   VpxSessionSource* self = static_cast<VpxSessionSource*>(userData);
   const uint64_t frame = ++self->m_renderedFrame;
   // Before Service, so a write already queued on the game's first frame is range-checked too.
   self->QueryAddressSpace();
   self->m_memory.Service(
      [self](uint32_t address, uint8_t* out, uint32_t size)
      {
         PinMAMEReadMemoryMsg msg { };
         msg.version = 1;
         msg.address = address;
         msg.size = size;
         msg.data = out;
         self->m_msgApi->BroadcastMsg(self->m_endpointId, self->m_readMemoryMsgId, &msg);
         return msg.read;
      },
      frame, self->m_playerRunning, self->m_gameRunning,
      [self](const std::vector<Wire::WriteOp>& ops)
      {
         std::vector<PinMAMEWriteMemoryOp> host;
         host.reserve(ops.size());
         for (const Wire::WriteOp& op : ops)
            host.push_back({ op.address, static_cast<uint32_t>(op.bytes.size()), op.bytes.data() });
         // Pre-filled with the sentinel: #634's handler always overwrites it (0 on a timeout),
         // while an unsubscribed broadcast leaves it untouched, as did a table stop before
         // pinmame 5fc234819, which answers 0 for that too.
         PinMAMEWriteMemoryMsg msg { 1, static_cast<uint32_t>(host.size()), host.data(), MemoryBridge::NO_WRITE_HANDLER };
         self->m_msgApi->BroadcastMsg(self->m_endpointId, self->m_writeMemoryMsgId, &msg);
         return msg.applied;
      });
   self->UpdateDeclare();
   self->PostStatusNotice();
}

void VpxSessionSource::QueryAddressSpace()
{
   if (m_addressSpaceKnown || !m_gameRunning)
      return;
   PinMAMEMachineStateMsg state { };
   state.version = 1;
   m_msgApi->BroadcastMsg(m_endpointId, m_machineStateMsgId, &state);
   if (state.hardwareGen == 0)
      return;
   m_addressSpaceKnown = true;
   m_memory.SetAddressSpace(MainCpuAddressSpace(state.hardwareGen));
}

void VpxSessionSource::TakeStatus()
{
   if (std::unique_ptr<StatusRecord> taken = m_statusLatch.Take())
   {
      m_scriptStatus = *taken;
      m_statusPending = std::move(taken);
   }
}

const StatusRecord& VpxSessionSource::ScriptStatus()
{
   TakeStatus();
   return m_scriptStatus;
}

ReadBackState& VpxSessionSource::ScriptReadBack()
{
   m_readBack.Drain(m_readBackChannel, std::chrono::steady_clock::now());
   return m_readBack;
}

void VpxSessionSource::PostStatusNotice()
{
   const auto now = std::chrono::steady_clock::now();
   const bool show = m_showStatus == nullptr || m_showStatus() != 0;
   TakeStatus();
   const std::string text = m_notifier.Frame(std::move(m_statusPending), show, now);
   if (text.empty() || m_vpxApi == nullptr)
      return;
   LOGI("Status notice: "s + text);
   // Updates ours in place while VPX still shows it, else posts a new one (vpinball #4228).
   m_noticeHandle = m_vpxApi->UpdateNotification(m_noticeHandle, text.c_str(), NOTICE_MS);
}

void VpxSessionSource::DeclareMachine(const std::string& machineId)
{
   // Logged and ignored rather than raised: a script error would stop the author's table.
   std::string canonical;
   if (!CanonicalMachineId(machineId, canonical))
   {
      // Escaped, so a newline from the script cannot forge a log line.
      std::string shown = machineId.substr(0, 80);
      for (char& c : shown)
         if (c < 0x20 || c > 0x7E)
            c = '?';
      // Once per table, as a script may call it from a timer.
      ScriptCallIgnored("Scorbit.Declare ignored, \""s + shown + "\" is not a machine id (a uuid, 8-4-4-4-12 hex digits)");
      return;
   }
   // Logged on a change only: a script may call it from a timer.
   if (canonical == m_authored.machineId)
      return;
   if (m_authored.machineId.empty())
      LOGI("script: Scorbit.Declare machine "s + canonical);
   else
   {
      LOGI("script: Scorbit.Declare replaces machine "s + m_authored.machineId + " with " + canonical);
      // A new epoch: what the old machine committed goes out under its own Declare first, and
      // what it called but never committed is dropped, never sent as the new machine's.
      m_epoch++;
      NewEpoch();
      m_ops.clear();
   }
   m_authored.machineId = std::move(canonical);
   UpdateDeclare();
}

bool VpxSessionSource::AuthoredReady() const
{
   return !m_authored.machineId.empty() && m_romId.empty() && !m_authored.tablePath.empty()
      && !m_authored.vpxPath.empty() && !m_authored.prefPath.empty();
}

void VpxSessionSource::ScriptCallIgnored(const std::string& why)
{
   if (m_loggedIgnoredCall)
      return;
   m_loggedIgnoredCall = true;
   LOGW("script: "s + why + " (logged once per table)");
}

void VpxSessionSource::ScriptGameOp(const Wire::GameOp& op)
{
   if (!AuthoredReady())
   {
      if (!m_loggedDroppedOps)
      {
         m_loggedDroppedOps = true;
         LOGW(m_romId.empty() ? "script: Scorbit game calls dropped until Scorbit.Declare names the machine and the game starts"s
                              : "script: Scorbit game calls dropped, a ROM is running ("s + m_romId + ")");
      }
      return;
   }
   if (m_ops.size() >= MAX_QUEUED_GAME_OPS)
   {
      ScriptCallIgnored("Scorbit game calls dropped, "s + std::to_string(m_ops.size()) + " are waiting for Commit");
      return;
   }
   m_ops.push_back(op);
}

void VpxSessionSource::ScriptCommit()
{
   if (m_ops.empty())
      return;
   std::vector<Wire::GameOp> ops;
   ops.swap(m_ops);
   // A ROM may have started between the calls and this commit.
   if (!AuthoredReady())
   {
      if (!m_loggedDroppedOps)
      {
         m_loggedDroppedOps = true;
         LOGW("script: Scorbit game calls dropped at Commit, the table is no longer an authored one"s);
      }
      return;
   }

   // A batch may hold game_start only first and game_end only last, and at most MAX_GAME_OPS,
   // so a commit is cut before each start, after each end, and at the size bound.
   std::vector<GamePost> posts;
   for (Wire::GameOp& op : ops)
   {
      if (posts.empty() || posts.back().batch.ops.size() == Wire::MAX_GAME_OPS
         || (op.kind == Wire::GAME_START && !posts.back().batch.ops.empty())
         || posts.back().batch.ops.back().kind == Wire::GAME_END)
      {
         posts.emplace_back();
         posts.back().epoch = m_epoch;
      }
      posts.back().batch.ops.push_back(std::move(op));
   }

   if (m_games.Push(std::move(posts)) && !m_loggedQueueDrop)
   {
      m_loggedQueueDrop = true;
      LOGW("script: Scorbit game calls dropped, the daemon is not taking them; the oldest games go first (logged once per table)"s);
   }
}

void VpxSessionSource::TakeGames(std::deque<GamePost>& out)
{
   m_games.TakeAll(out);
}

void VpxSessionSource::SetRomId(const std::string& romId)
{
   m_romId = romId;
   m_addressSpaceKnown = false;
   m_memory.SetAddressSpace(0);
   m_gameRunning = !romId.empty();
   m_memory.SetGameRunning(!romId.empty());
   UpdateDeclare();
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

void VpxSessionSource::Refresh(const DmdTap& tap, TapView& view)
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
   const bool tapHasSource = tap.HasSource();
   // A torn copy keeps the frame this view already held: it is the source that changed,
   // not the display, and treating a tear as "no display" would re-declare a 0x0 one.
   const bool haveFrame = tap.GetLatest(view.latest, view.scratch) || (tap.HasFrame() && view.latest.width != 0);
   const uint64_t tapGeneration = tap.SourceGeneration();
   view.hasSource = tapHasSource && haveFrame && view.latest.generation == tapGeneration;

   // During that gap the daemon still sees the generation move, so it discards
   // its cache and stops treating the frame id it last saw as current.
   view.generation = view.hasSource ? view.latest.generation : tapGeneration;
}

void VpxSessionSource::UpdateDeclare()
{
   Refresh(m_tap, m_apiTap);
   const DmdTap::Frame& f = m_apiTap.latest;
   const bool has = m_apiTap.hasSource;

   DeclareSnapshot now;
   Wire::Declare& d = now.declare;
   d.romId = m_romId;
   d.tablePath = m_tablePath;
   d.vpxVersion = SCORBIT_VPX_PINNED_VERSION;
   d.vpxRevision = SCORBIT_VPX_PINNED_REVISION;
   d.width = static_cast<uint16_t>(has ? f.width : 0);
   d.height = static_cast<uint16_t>(has ? f.height : 0);
   d.shades = static_cast<uint8_t>(has ? f.shades : 0);
   d.sourceGeneration = static_cast<uint32_t>(m_apiTap.generation);
   d.identifyFormat = !has ? "" : (f.shades == 16 ? "BITPLANE4" : "BITPLANE2");
   if (AuthoredReady())
   {
      d.tableKind = Wire::TABLE_AUTHORED;
      d.machineUuid = m_authored.machineId;
      d.tableFullPath = m_authored.tablePath;
      d.vpxAppPath = m_authored.vpxPath;
      d.prefsPath = m_authored.prefPath;
   }
   else if (!m_authored.machineId.empty() && !m_romId.empty() && !m_loggedRomWins)
   {
      // A hybrid table: memory extraction is the trusted path, and both would double-report.
      m_loggedRomWins = true;
      LOGI("script: Scorbit.Declare ignored, a ROM is running ("s + m_romId + ")");
   }
   now.epoch = m_epoch;
   now.surfaceGeneration = m_overlay.SurfaceGeneration();
   // A new identity in the same epoch (a hybrid table's ROM taking over): the worker drops the
   // players and the old identity's requests; the results already read for it go here.
   if (m_havePosted && now.epoch == m_posted.epoch && Wire::NewIdentity(m_posted.declare, now.declare))
   {
      m_identity++;
      m_readBack.ClearScores();
   }
   now.identity = m_identity;
   if (m_havePosted && now == m_posted)
      return;
   m_posted = now;
   m_havePosted = true;
   m_declares.Post(now);
}

void VpxSessionSource::GetFrame(FrameSnapshot& out)
{
   Refresh(m_tap, m_workerTap);
   const DmdTap::Frame& latest = m_workerTap.latest;

   out.hasSource = m_workerTap.hasSource;
   out.generation = static_cast<uint32_t>(m_workerTap.generation);
   if (!m_workerTap.hasSource)
   {
      out.frameId = 0;
      out.width = 0;
      out.height = 0;
      out.shades = 0;
      out.pixels.clear();
      return;
   }
   out.frameId = latest.frameId;
   out.width = static_cast<uint16_t>(latest.width);
   out.height = static_cast<uint16_t>(latest.height);
   out.shades = static_cast<uint8_t>(latest.shades);
   // Handed over exactly as the tap captured it: one shade index per pixel,
   // row major. Nothing repacks it on the way to the daemon.
   out.pixels = latest.pixels;
}

void VpxSessionSource::GetSession(SessionSnapshot& out)
{
   out.renderedFrame = m_renderedFrame;
   out.playerRunning = m_playerRunning;
}

}
