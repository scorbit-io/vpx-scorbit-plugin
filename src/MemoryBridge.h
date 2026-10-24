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
// Service takes the queue with a try_lock, so a contended frame is skipped and
// picked up on the next one, and reads with the lock released, so a worker at its
// deadline never waits behind a read. The snapshot is published through a seqlock,
// so a Poll never waits on the API thread's copy, nor it on the Poll. Requests are shared, so one the worker
// gave up on (timeout, unload) is still safe for the API thread to complete; it
// is only never installed.

#include "SeqLock.h"
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
   // Applies the ops in order as one batch and returns how many were applied: NO_WRITE_HANDLER
   // when nothing handled the message at all (a libpinmame without pinmame #634), 0 when a
   // handler applied nothing (its wait timed out, the emulator being paused or stalled).
   using WriteFn = std::function<uint32_t(const std::vector<Wire::WriteOp>& ops)>;
   static constexpr uint32_t NO_WRITE_HANDLER = 0xFFFFFFFFu;

   enum class Result
   {
      Ok,
      Busy,        // the API thread did not answer in time, or the queue is full
      NoGame,
      OutOfRange,
      TooLarge,
      Unsupported, // the host has no write handler; remembered until the next Clear
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

   // One Write: its ops applied in order in one pass. budgetMs bounds the wait for VPX's
   // thread to take it, and a write not taken by then answers Busy with nothing applied.
   // Once taken, the worker waits for the real answer, which the host bounds (pinmame #634
   // gives up at 250 ms); only a stop ends that wait. A taken write is never answered Busy,
   // because it may still be applied, and a daemon that retried would apply it twice. A host with no write handler answers Unsupported, and every Write until the
   // next Clear then answers it at once; a handler that applied nothing answers Busy and is
   // not remembered, since it is transient. Per session rather than for the plugin's life,
   // because a table that is stopping also leaves the message unhandled.
   Result Write(const std::vector<Wire::WriteOp>& ops, Wire::WriteReply& out, int budgetMs,
      const std::atomic<bool>& running);

   // Starts a session's memory state over: a new session and every Declare start from no
   // subscription, and forget an Unsupported write (the next table may be able to write).
   // An empty Subscribe only empties the subscription.
   void Clear();

   // --- API thread ------------------------------------------------------------

   // When PinMAME starts or stops running a ROM. Service stops being called once
   // the player closes, so it cannot be what tells the worker there is no game;
   // stopping also answers whatever is still queued.
   void SetGameRunning(bool running);

   // Once per rendered frame. An empty write function answers writes Unsupported.
   void Service(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning,
      const WriteFn& write);

   // Tests only: requests waiting for Service.
   size_t QueuedForTest();

   // Requests allowed to wait at once; a full queue answers Busy at once.
   static constexpr size_t QUEUE_LIMIT = 64;

private:
   struct Request
   {
      enum class Kind { Subscribe, ReadDirect, Write } kind = Kind::ReadDirect;
      std::vector<Wire::MemoryRange> ranges;   // Subscribe
      uint32_t address = 0;                    // ReadDirect
      uint32_t length = 0;
      uint64_t subscriptionId = 0;             // Subscribe: the id to install
      std::vector<Wire::WriteOp> ops;          // Write

      // Written by the API thread before done is set.
      std::vector<Wire::MemoryRange> accepted;
      Wire::ReadDirectReply reply;
      Wire::WriteReply writeReply;
      Result result = Result::Busy;

      // Under m_mutex.
      bool done = false;
      bool abandoned = false;   // the worker answered busy; never install it
      bool taken = false;       // Service swapped it out; a write is then waited on to the end
   };

   using Queue = std::deque<std::shared_ptr<Request>>;

   Result Submit(const std::shared_ptr<Request>& request, int budgetMs, const std::atomic<bool>& running);
   // Under m_mutex: empties the subscription and invalidates its snapshot.
   void ResetSubscription();
   // Reads only; touches no shared state, so it runs without the lock.
   static void Answer(Request& request, const ReadFn& read, const WriteFn& write, uint64_t frame,
      bool gameRunning);
   // Under m_mutex.
   void Complete(Queue& requests);
   // Reads only, into m_nextSnapshot; true when every range read in full.
   bool TakeSnapshot(const ReadFn& read, const std::vector<Wire::MemoryRange>& ranges, uint64_t frame,
      bool playerRunning, bool gameRunning);
   // API thread. Publishes m_nextSnapshot, valid for the subscription rangesId when complete.
   void PublishSnapshot(uint64_t rangesId, bool complete);

   std::mutex m_mutex;
   std::condition_variable m_answered;
   Queue m_queue;

   // The subscription, under m_mutex. Its id ties a snapshot to the subscription it
   // was taken for, so a Poll never answers with bytes of a set since replaced.
   std::vector<Wire::MemoryRange> m_ranges;
   uint64_t m_subscriptionId = 0;
   uint64_t m_nextSubscriptionId = 1;

   // The snapshot: a header, every range's address and length, then their bytes.
   struct SnapshotHeader
   {
      uint64_t frame;
      uint64_t rangesId;
      uint32_t blocks;
      uint8_t playerRunning;
      uint8_t complete;   // every range read in full, for rangesId
   };
   static constexpr size_t SNAPSHOT_BYTES = sizeof(SnapshotHeader) + Wire::MAX_SUBSCRIBE_RANGES * 8 + Wire::MAX_SUBSCRIBE_BYTES;
   Wire::PollReply m_nextSnapshot;    // API thread only
   std::mutex m_publishMutex;         // writers only, should a second thread ever publish
   std::vector<uint8_t> m_publishBuf; // under m_publishMutex
   SeqLock<SNAPSHOT_BYTES> m_snapshot;
   std::vector<uint8_t> m_pollBuf;    // worker only

   std::atomic<bool> m_gameRunning { false };
   std::atomic<bool> m_writesUnsupported { false };
};

}
