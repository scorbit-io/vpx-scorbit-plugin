// license:GPLv3+

#pragma once

// Carries slice 2 memory reads between the socket worker and VPX's API thread.
//
// The plugin API is not thread safe, so the worker never reads memory itself.
// It queues a request here and waits, bounded; the API thread calls Service
// once per rendered frame, which answers the queue and refreshes the snapshot
// of the subscribed ranges that Poll is served from (scorbitd design.md,
// "Threading and the REQ-8.3 handoff").
//
// Service only ever try_locks, so the render path never waits on the worker: a
// contended frame is skipped and picked up on the next one. Requests are shared
// with the queue, so one the worker gave up on (timeout, unload) is still safe
// for the API thread to complete.

#include "WireProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace Scorbit
{

class MemoryBridge final
{
public:
   // Reads main CPU memory; returns the bytes actually read, 0 when unavailable.
   using ReadFn = std::function<uint32_t(uint32_t address, uint8_t* out, uint32_t size)>;

   enum class Result
   {
      Ok,
      Busy,        // the API thread did not answer in time, or the queue is full
      NoGame,
      OutOfRange,
      TooLarge,
   };

   // --- worker thread ---------------------------------------------------------

   // Replaces the subscription. The ranges it can read in full, in request order,
   // come back in accepted; the rest are dropped, which the daemon allows.
   Result Subscribe(const std::vector<Wire::MemoryRange>& ranges, std::vector<Wire::MemoryRange>& accepted,
      int budgetMs, const std::atomic<bool>& running);

   // The latest snapshot of the subscribed set, exactly and in order.
   Result Poll(Wire::PollReply& out);

   // One range, each 256-byte chunk read twice (pinmame #642). Short only when
   // memory past some point cannot be read.
   Result ReadDirect(uint32_t address, uint32_t length, Wire::ReadDirectReply& out, int budgetMs,
      const std::atomic<bool>& running);

   // Empties the subscription: a new session and every Declare start from none.
   void Clear();

   // --- API thread ------------------------------------------------------------

   // When PinMAME starts or stops running a ROM. Service stops being called once
   // the player closes, so it cannot be what tells the worker there is no game.
   void SetGameRunning(bool running);

   // Once per rendered frame.
   void Service(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning);

   // Tests only: requests waiting for Service.
   size_t QueuedForTest();

   // Requests allowed to wait at once; a full queue answers Busy at once.
   static constexpr size_t QUEUE_LIMIT = 64;

private:
   struct Request
   {
      enum class Kind { Subscribe, ReadDirect } kind = Kind::ReadDirect;
      std::vector<Wire::MemoryRange> ranges;   // Subscribe
      uint32_t address = 0;                    // ReadDirect
      uint32_t length = 0;
      uint64_t subscriptionId = 0;             // Subscribe: the id to install

      // Written by the API thread before done is set.
      std::vector<Wire::MemoryRange> accepted;
      Wire::ReadDirectReply reply;
      Result result = Result::Busy;
      std::atomic<bool> done { false };
   };

   Result Submit(const std::shared_ptr<Request>& request, int budgetMs, const std::atomic<bool>& running);
   void Answer(Request& request, const ReadFn& read, uint64_t frame, bool gameRunning);
   void TakeSnapshot(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning);

   std::mutex m_mutex;
   std::condition_variable m_answered;
   std::deque<std::shared_ptr<Request>> m_queue;

   // The subscription and the snapshot of it, under m_mutex. An id ties a
   // snapshot to the subscription it was taken for, so a Poll never answers with
   // bytes of a set that has since been replaced.
   std::vector<Wire::MemoryRange> m_ranges;
   uint64_t m_subscriptionId = 0;
   uint64_t m_nextSubscriptionId = 1;
   bool m_haveSnapshot = false;
   uint64_t m_snapshotId = 0;
   Wire::PollReply m_snapshot;

   std::atomic<bool> m_gameRunning { false };
};

}
