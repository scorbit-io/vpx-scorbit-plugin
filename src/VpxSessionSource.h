// license:GPLv3+

#pragma once

// Everything the socket worker needs to know about the running game, captured
// on the Visual Pinball API thread and read back from the worker thread.
//
// The worker must never touch the plugin bus: VPX fires an async callback
// before erasing it, so a nested pump can run a callback twice. So the API
// thread pushes here (romId when the emulating controller changes, table path
// and player state on game start and end, one tick per rendered frame) and the
// worker only reads: the declaration through the Declare latch, frames through
// the tap's seqlock, the rest through atomics. Neither thread waits on the other.
//
// The same rule is why log lines from the worker are queued rather than
// written: LoggingPlugin makes no thread safety promise, so the queue is
// drained back on the API thread. Status comes back the same way, through a
// latch the API thread drains once per rendered frame into a notification.

#include "AuthoredTable.h"
#include "DmdOverlay.h"
#include "DmdTap.h"
#include "MemoryBridge.h"
#include "SocketWorker.h"
#include "StatusNotice.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace Scorbit
{

class VpxSessionSource final : public ISessionSource, public IOverlayTarget
{
public:
   // showStatus reads [Plugin.Scorbit] ShowStatus; it is read every frame, so a change applies at once.
   VpxSessionSource(const MsgPluginAPI* msgApi, uint32_t endpointId, VPXPluginAPI* vpxApi, DmdTap& tap, DmdOverlay& overlay,
      int (*showStatus)());
   ~VpxSessionSource() override;

   VpxSessionSource(const VpxSessionSource&) = delete;
   VpxSessionSource& operator=(const VpxSessionSource&) = delete;

   // API thread only. Empty means no game.
   void SetRomId(const std::string& romId);

   // API thread only: the table script's Scorbit.Declare (SB-5990). The script runs before
   // OnGameStart, so the id is kept until the game ends; a later call replaces it. Held
   // host-side until the authored Declare has a wire layout (SB-5984).
   void DeclareMachine(const std::string& machineId);
   const AuthoredTable& Authored() const { return m_authored; }

   // API thread only. Builds the declaration and posts it to the worker when it
   // changed. Called per rendered frame and from the plugin's timer, so it stays
   // current while no frame renders.
   void UpdateDeclare();

   // API thread only. Writes out everything the worker logged since the last
   // call. Cheap when the queue is empty, which is almost always.
   void DrainLog();

   // Any thread. The socket worker's log sink.
   void PushLog(int level, const std::string& message);

   // The slice 2 memory reads, serviced here once per rendered frame. Owned here
   // so it outlives the socket worker, which is deleted first at unload.
   MemoryBridge& Memory() { return m_memory; }

   // The socket worker's half of the Status latch. Owned here for the same reason.
   StatusLatch& Status() { return m_statusLatch; }

   // ISessionSource, worker thread.
   std::unique_ptr<DeclareSnapshot> TakeDeclare() override { return m_declares.Take(); }
   void GetFrame(FrameSnapshot& out) override;
   void GetSession(SessionSnapshot& out) override;

   // IOverlayTarget, worker thread.
   bool OverlaySurface() override { return m_overlay.HasSurface(); }
   Overlay::State::Upload OverlayUpload(const uint8_t* payload, size_t size) override { return m_overlay.Store(payload, size); }
   void OverlayControl(bool enable, uint32_t durationMs) override { m_overlay.Control(enable, durationMs); }
   void OverlayClear() override { m_overlay.Clear(); }

private:
   static void OnGameStart(const unsigned int msgId, void* userData, void* msgData);
   static void OnGameEnd(const unsigned int msgId, void* userData, void* msgData);
   static void OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData);

   // What one thread knows of the tap. Each thread keeps its own.
   struct TapView
   {
      DmdTap::Frame latest;
      std::vector<uint8_t> scratch;
      bool hasSource = false;
      // DmdTap's own counter, which bumps on every source change including a
      // replacement whose geometry is identical. The wire carries its low 32 bits:
      // it counts source changes within one plugin load, so it would have to wrap
      // a u64 before that could hide one.
      uint64_t generation = 0;
   };
   // Pulls the tap and derives the source generation.
   static void Refresh(const DmdTap& tap, TapView& view);
   // API thread, once per rendered frame: notifications are drawn inside rendered
   // frames and timed from their push, so one pushed while none render expires unseen.
   void PostStatusNotice();
   // API thread, once per rendered frame until PinMAME answers: the machine's address space.
   void QueryAddressSpace();

   const MsgPluginAPI* const m_msgApi;
   const uint32_t m_endpointId;
   VPXPluginAPI* const m_vpxApi;
   DmdTap& m_tap;
   DmdOverlay& m_overlay;
   int (*const m_showStatus)();
   const unsigned int m_gameStartMsgId;
   const unsigned int m_gameEndMsgId;
   const unsigned int m_prepareFrameMsgId;
   const unsigned int m_readMemoryMsgId;
   const unsigned int m_writeMemoryMsgId;
   const unsigned int m_machineStateMsgId;

   std::atomic<uint64_t> m_renderedFrame { 0 };
   std::atomic<bool> m_playerRunning { false };
   // A ROM is running in PinMAME: SetRomId's argument is not empty.
   std::atomic<bool> m_gameRunning { false };
   MemoryBridge m_memory;

   // API thread only.
   std::string m_romId;
   std::string m_tablePath;
   uint64_t m_epoch = 0;
   TapView m_apiTap;
   DeclareSnapshot m_posted;
   bool m_havePosted = false;
   bool m_addressSpaceKnown = false;
   AuthoredTable m_authored;

   Latch<DeclareSnapshot> m_declares;
   TapView m_workerTap; // worker only

   StatusLatch m_statusLatch;
   StatusNotifier m_notifier; // API thread only, as is the handle
   unsigned int m_noticeHandle = 0;

   std::mutex m_logMutex;
   std::deque<std::pair<int, std::string>> m_logQueue;
   uint64_t m_logDropped = 0;
};

}
