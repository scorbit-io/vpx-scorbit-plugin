// license:GPLv3+

#pragma once

// The overlay cache (docs/slice4-internals.md): two writers, the socket worker and the API
// thread's in-process Overlay message, and any number of render threads reading.
//
// Writers serialise on a mutex that readers never take, and publish each result as one
// fixed record through a seqlock, so a render thread never waits on a writer. The API
// thread never waits on the worker either: it only try_locks, and otherwise queues the op,
// in arrival order, for its next frame.

#include "OverlayState.h"
#include "SeqLock.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace Scorbit::Overlay
{

// One published state. No pointers, so a copy is the whole value.
struct Record
{
   uint32_t width = 0;
   uint32_t height = 0;
   uint32_t enabled = 0;
   uint32_t untilHidden = 0;
   int64_t hideAt = 0;  // steady_clock ticks
   uint8_t pixels[MAX_WIDTH * MAX_HEIGHT] { };

   // Judged by each reader on every frame, so the timer needs no writer to expire.
   bool Visible(State::Clock::time_point now) const
   {
      return enabled != 0 && width != 0 && (untilHidden != 0 || now.time_since_epoch().count() < hideAt);
   }
};

// What a record carries before its pixels, of which only width * height are published.
inline constexpr size_t RECORD_HEADER_BYTES = offsetof(Record, pixels);

class Cache final
{
public:
   using Clock = State::Clock;

   // Ops the API thread may hold for its next frame; one more answers refused.
   static constexpr size_t PENDING_LIMIT = 8;

   Cache() { Publish(); }
   Cache(const Cache&) = delete;
   Cache& operator=(const Cache&) = delete;

   // --- socket worker: waits only for the API thread's copy into the record ---

   State::Upload Store(const uint8_t* payload, size_t size)
   {
      std::lock_guard lock(m_writer);
      const State::Upload result = m_state.Store(payload, size);
      if (result == State::Upload::Stored)
         Publish();
      return result;
   }

   void Control(bool enable, uint32_t durationMs)
   {
      std::lock_guard lock(m_writer);
      m_state.Control(enable, durationMs, Clock::now());
      Publish();
   }

   void Clear()
   {
      std::lock_guard lock(m_writer);
      m_state.Clear();
      Publish();
   }

   // --- API thread: never waits -----------------------------------------------

   // True when accepted: decided from the payload alone, not from whether it applied yet.
   bool PostUpload(const uint8_t* payload, size_t size)
   {
      if (State::Check(payload, size) != State::Upload::Stored)
         return false;
      Op op;
      op.upload = true;
      op.bytes.assign(payload, payload + size);
      return Post(std::move(op));
   }

   bool PostControl(bool enable, uint32_t durationMs)
   {
      Op op;
      op.enable = enable;
      op.durationMs = durationMs;
      op.at = Clock::now();
      return Post(std::move(op));
   }

   // Once per rendered frame. Nothing renders while nothing drains, so no timer is needed.
   void DrainPending()
   {
      if (m_pending.empty())
         return;
      std::unique_lock lock(m_writer, std::try_to_lock);
      if (!lock.owns_lock())
         return;
      while (!m_pending.empty())
      {
         Apply(m_pending.front());
         m_pending.pop_front();
      }
      Publish();
   }

   size_t PendingForTest() const { return m_pending.size(); }
   std::unique_lock<std::mutex> HoldWritersForTest() { return std::unique_lock<std::mutex>(m_writer); }

   // --- render threads, any number at once ----------------------------------------

   // Copies the record, through the caller's scratch, when it changed since lastSequence,
   // which it then updates. False when it did not change, or every copy tore; out is then
   // untouched, so the caller keeps its last good copy.
   bool ReadIfChanged(uint64_t& lastSequence, Record& out, Record& scratch) const
   {
      if (m_published.Sequence() == lastSequence)
         return false;
      size_t size = 0;
      uint64_t sequence = 0;
      if (!m_published.Read(&scratch, size, &sequence) || size < RECORD_HEADER_BYTES)
         return false;
      std::memcpy(&out, &scratch, size);
      lastSequence = sequence;
      return true;
   }

private:
   struct Op
   {
      bool upload = false;
      std::vector<uint8_t> bytes;
      bool enable = false;
      uint32_t durationMs = 0;
      Clock::time_point at { };  // the timer counts from arrival, not from the drain
   };

   bool Post(Op&& op)
   {
      // A non-empty queue takes every new op, so a later show can never overtake a queued upload.
      if (m_pending.empty())
      {
         std::unique_lock lock(m_writer, std::try_to_lock);
         if (lock.owns_lock())
         {
            Apply(op);
            Publish();
            return true;
         }
      }
      if (m_pending.size() >= PENDING_LIMIT)
         return false;
      m_pending.push_back(std::move(op));
      return true;
   }

   // Under m_writer.
   void Apply(const Op& op)
   {
      if (op.upload)
         m_state.Store(op.bytes.data(), op.bytes.size());
      else
         m_state.Control(op.enable, op.durationMs, op.at);
   }

   // Under m_writer, or from the constructor.
   void Publish()
   {
      m_scratch.width = m_state.Width();
      m_scratch.height = m_state.Height();
      m_scratch.enabled = m_state.Enabled() ? 1 : 0;
      m_scratch.untilHidden = m_state.UntilHidden() ? 1 : 0;
      m_scratch.hideAt = static_cast<int64_t>(m_state.HideAt().time_since_epoch().count());
      const std::vector<uint8_t>& pixels = m_state.Pixels();
      std::copy(pixels.begin(), pixels.end(), m_scratch.pixels);
      m_published.Write(&m_scratch, RECORD_HEADER_BYTES + pixels.size());
   }

   std::mutex m_writer;
   State m_state;              // under m_writer
   Record m_scratch;           // under m_writer
   SeqLock<sizeof(Record)> m_published;
   std::deque<Op> m_pending;   // API thread only
};

}
