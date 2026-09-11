// license:GPLv3+

#pragma once

// Everything the socket worker needs to know about the running game, captured
// on the Visual Pinball API thread and read back from the worker thread.
//
// The worker must never touch the plugin bus: VPX fires an async callback
// before erasing it, so a nested pump can run a callback twice. So the API
// thread pushes here (romId when the emulating controller changes, table path
// and player state on game start and end, one tick per rendered frame) and the
// worker only reads, under this object's own lock or through an atomic.
//
// The same rule is why log lines from the worker are queued rather than
// written: LoggingPlugin makes no thread safety promise, so the queue is
// drained back on the API thread.

#include "DmdTap.h"
#include "SocketWorker.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace Scorbit
{

class VpxSessionSource final : public ISessionSource
{
public:
   VpxSessionSource(const MsgPluginAPI* msgApi, uint32_t endpointId, VPXPluginAPI* vpxApi, DmdTap& tap);
   ~VpxSessionSource() override;

   VpxSessionSource(const VpxSessionSource&) = delete;
   VpxSessionSource& operator=(const VpxSessionSource&) = delete;

   // API thread only. Empty means no game.
   void SetRomId(const std::string& romId);

   // API thread only. Writes out everything the worker logged since the last
   // call. Cheap when the queue is empty, which is almost always.
   void DrainLog();

   // Any thread. The socket worker's log sink.
   void PushLog(int level, const std::string& message);

   // ISessionSource, worker thread.
   void GetDeclare(DeclareSnapshot& out) override;
   void GetFrame(FrameSnapshot& out) override;
   void GetSession(SessionSnapshot& out) override;

private:
   static void OnGameStart(const unsigned int msgId, void* userData, void* msgData);
   static void OnGameEnd(const unsigned int msgId, void* userData, void* msgData);
   static void OnPrepareFrame(const unsigned int msgId, void* userData, void* msgData);

   // Pulls the tap and maintains the derived source generation. Call under m_mutex.
   void Refresh();

   const MsgPluginAPI* const m_msgApi;
   const uint32_t m_endpointId;
   VPXPluginAPI* const m_vpxApi;
   DmdTap& m_tap;
   const unsigned int m_gameStartMsgId;
   const unsigned int m_gameEndMsgId;
   const unsigned int m_prepareFrameMsgId;

   std::atomic<uint64_t> m_renderedFrame { 0 };
   std::atomic<bool> m_playerRunning { false };

   std::mutex m_mutex;
   std::string m_romId;
   std::string m_tablePath;
   uint64_t m_epoch = 0;

   DmdTap::Frame m_latest;
   bool m_hasSource = false;
   // DmdTap's own counter, which bumps on every source change including a
   // replacement whose geometry is identical. The wire carries its low 32 bits:
   // it counts source changes within one plugin load, so it would have to wrap
   // a u64 before that could hide one.
   uint64_t m_generation = 0;

   std::mutex m_logMutex;
   std::deque<std::pair<int, std::string>> m_logQueue;
   uint64_t m_logDropped = 0;
};

}
