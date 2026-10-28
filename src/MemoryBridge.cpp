// license:GPLv3+

#include "MemoryBridge.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Scorbit
{

namespace
{

// True when [address, address + length) wraps the 32-bit address space.
bool Wraps(uint32_t address, uint32_t length)
{
   return static_cast<uint64_t>(address) + length > 0x100000000ULL;
}

}

MemoryBridge::Result MemoryBridge::Subscribe(const std::vector<Wire::MemoryRange>& ranges,
   std::vector<Wire::MemoryRange>& accepted, int budgetMs, const std::atomic<bool>& running)
{
   accepted.clear();
   if (ranges.size() > Wire::MAX_SUBSCRIBE_RANGES)
      return Result::TooLarge;
   uint64_t total = 0;
   for (const Wire::MemoryRange& range : ranges)
   {
      total += range.length;
      if (Wraps(range.address, range.length))
         return Result::OutOfRange;
   }
   if (total > Wire::MAX_SUBSCRIBE_BYTES)
      return Result::TooLarge;

   // Nothing is readable with no ROM running, and Service may not be running to
   // say so: answer at once with nothing kept, which the daemon allows.
   if (ranges.empty() || !m_gameRunning)
   {
      std::lock_guard lock(m_mutex);
      ResetSubscription();
      return Result::Ok;
   }

   auto request = std::make_shared<Request>();
   request->kind = Request::Kind::Subscribe;
   request->ranges = ranges;
   {
      std::lock_guard lock(m_mutex);
      request->subscriptionId = m_nextSubscriptionId++;
   }
   const Result result = Submit(request, budgetMs, running);
   if (result == Result::Ok)
      accepted = std::move(request->accepted);
   return result;
}

MemoryBridge::Result MemoryBridge::Poll(Wire::PollReply& out)
{
   if (!m_gameRunning)
      return Result::NoGame;
   uint64_t subscriptionId;
   bool unsubscribed;
   {
      std::lock_guard lock(m_mutex);
      subscriptionId = m_subscriptionId;
      unsubscribed = m_ranges.empty();
   }

   // Torn more than the seqlock's retries allow is "not available for this poll".
   SnapshotHeader header { };
   size_t got = 0;
   const bool read = m_snapshot.Read(m_pollBuf, got) && got >= sizeof(header);
   if (read)
      std::memcpy(&header, m_pollBuf.data(), sizeof(header));
   if (unsubscribed)
   {
      out = { };
      out.frame = read ? header.frame : 0;
      out.playerRunning = read ? header.playerRunning : 0;
      return Result::Ok;
   }
   if (!read || !header.complete || header.rangesId != subscriptionId)
      return Result::Busy;

   out = { };
   out.frame = header.frame;
   out.playerRunning = header.playerRunning;
   out.blocks.resize(header.blocks);
   size_t at = sizeof(header);
   for (Wire::MemoryBlock& block : out.blocks)
   {
      uint32_t length;
      std::memcpy(&block.address, m_pollBuf.data() + at, 4);
      std::memcpy(&length, m_pollBuf.data() + at + 4, 4);
      block.bytes.resize(length);
      at += 8;
   }
   for (Wire::MemoryBlock& block : out.blocks)
   {
      if (at + block.bytes.size() > got)
         return Result::Busy;
      std::memcpy(block.bytes.data(), m_pollBuf.data() + at, block.bytes.size());
      at += block.bytes.size();
   }
   return Result::Ok;
}

MemoryBridge::Result MemoryBridge::ReadDirect(uint32_t address, uint32_t length, Wire::ReadDirectReply& out,
   int budgetMs, const std::atomic<bool>& running)
{
   if (length > Wire::MAX_READ_DIRECT_BYTES)
      return Result::TooLarge;
   if (length == 0 || Wraps(address, length))
      return Result::OutOfRange;

   // SetGameRunning keeps this current even while no frame is rendering, so a
   // table chooser gets no_game at once instead of a busy the daemon would retry.
   if (!m_gameRunning)
      return Result::NoGame;

   auto request = std::make_shared<Request>();
   request->kind = Request::Kind::ReadDirect;
   request->address = address;
   request->length = length;
   const Result result = Submit(request, budgetMs, running);
   if (result == Result::Ok)
      out = std::move(request->reply);
   return result;
}

MemoryBridge::Result MemoryBridge::Write(const std::vector<Wire::WriteOp>& ops, Wire::WriteReply& out,
   int budgetMs, const std::atomic<bool>& running)
{
   if (ops.size() > Wire::MAX_WRITE_OPS)
      return Result::TooLarge;
   if (ops.empty())
      return Result::OutOfRange;
   const uint64_t space = m_addressSpace;
   for (const Wire::WriteOp& op : ops)
   {
      if (Wraps(op.address, static_cast<uint32_t>(op.bytes.size())))
         return Result::OutOfRange;
      if (space != 0 && !op.bytes.empty() && op.address + uint64_t { op.bytes.size() } > space)
         return Result::OutOfRange;
   }
   if (m_writesUnsupported)
      return Result::Unsupported;
   if (!m_gameRunning)
      return Result::NoGame;

   auto request = std::make_shared<Request>();
   request->kind = Request::Kind::Write;
   request->ops = ops;
   const Result result = Submit(request, budgetMs, running);
   if (result == Result::Ok)
      out = request->writeReply;
   return result;
}

void MemoryBridge::Clear()
{
   std::lock_guard lock(m_mutex);
   m_writesUnsupported = false;
   ResetSubscription();
}

void MemoryBridge::ResetSubscription()
{
   m_ranges.clear();
   // A fresh id, so a Subscribe already queued from before this point cannot
   // install its set once the API thread gets to it.
   m_subscriptionId = m_nextSubscriptionId++;
}

MemoryBridge::Result MemoryBridge::Submit(const std::shared_ptr<Request>& request, int budgetMs,
   const std::atomic<bool>& running)
{
   {
      std::lock_guard lock(m_mutex);
      // Checked again here, under the lock SetGameRunning drains the queue with:
      // a game ending since the caller's check left no Service to answer this.
      if (!m_gameRunning)
      {
         Answer(*request, { }, { }, 0, false);
         Queue one { request };
         Complete(one);
         return request->result;
      }
      if (m_queue.size() >= QUEUE_LIMIT)
         return Result::Busy;
      m_queue.push_back(request);
   }

   // Woken by Service; the short slice is only so a stopping worker notices. A
   // plain sleep would add up to 16 ms per request on Windows' default timer.
   // Service never reads under the lock, so waking up late is never a read's doing.
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
   std::unique_lock lock(m_mutex);
   const auto waiting = [&]
   {
      if (request->done || !running)
         return false;
      // A write already taken may still be applied: answering busy would report a failure
      // that did not happen, and a retry would apply it twice. The host bounds the wait.
      return (request->kind == Request::Kind::Write && request->taken)
         || std::chrono::steady_clock::now() < deadline;
   };
   while (waiting())
   {
      // Wake at the budget, not on the next 5 ms tick past it.
      const auto tick = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
      m_answered.wait_until(lock, tick < deadline || request->taken ? tick : deadline);
   }

   // Decided under the lock Complete runs under: either it finished the request,
   // or it never will install it. The request may already be out of the queue and
   // mid-read on the API thread, which is why this marks it rather than erasing only.
   if (request->done)
      return request->result;
   request->abandoned = true;
   m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), request), m_queue.end());
   return Result::Busy;
}

void MemoryBridge::SetGameRunning(bool running)
{
   {
      std::lock_guard lock(m_mutex);
      m_gameRunning = running;
      if (running)
         return;
      // No Service is coming to answer these, so answer them now: without a game
      // nothing is read, and each gets its no-game answer.
      Queue pending;
      pending.swap(m_queue);
      for (const std::shared_ptr<Request>& request : pending)
         Answer(*request, { }, { }, 0, false);
      Complete(pending);
   }
   // No snapshot outlives the game, so a restart cannot serve the last one's bytes.
   m_nextSnapshot.playerRunning = 0;
   m_nextSnapshot.blocks.clear();
   PublishSnapshot(0, false);
   m_answered.notify_all();
}

size_t MemoryBridge::QueuedForTest()
{
   std::lock_guard lock(m_mutex);
   return m_queue.size();
}

void MemoryBridge::Service(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning,
   const WriteFn& write)
{
   Queue work;
   std::vector<Wire::MemoryRange> ranges;
   uint64_t rangesId = 0;
   {
      std::unique_lock lock(m_mutex, std::try_to_lock);
      if (!lock.owns_lock())
         return;
      m_gameRunning = gameRunning;
      work.swap(m_queue);
      for (const std::shared_ptr<Request>& request : work)
         request->taken = true;
      ranges = m_ranges;
      rangesId = m_subscriptionId;
   }

   for (const std::shared_ptr<Request>& request : work)
   {
      Answer(*request, read, write, frame, gameRunning);
      // Snapshot the set this batch installs, so a Poll right after the
      // Subscribe reply already has it.
      if (request->kind == Request::Kind::Subscribe && request->subscriptionId > rangesId)
      {
         ranges = request->accepted;
         rangesId = request->subscriptionId;
      }
   }
   const bool haveSnapshot = TakeSnapshot(read, ranges, frame, playerRunning, gameRunning);
   // Before the requests complete, so a Poll right after a Subscribe reply has its set.
   // Tagged with the set it was read for: a Poll matches it against the installed one, so
   // one replaced by Clear meanwhile, or an abandoned Subscribe's, is never served.
   PublishSnapshot(rangesId, haveSnapshot);

   {
      // Blocking, but the worker only ever holds this for bookkeeping, never a read.
      std::lock_guard lock(m_mutex);
      Complete(work);
   }
   m_answered.notify_all();
}

void MemoryBridge::PublishSnapshot(uint64_t rangesId, bool complete)
{
   std::lock_guard lock(m_publishMutex);
   const SnapshotHeader header { m_nextSnapshot.frame, rangesId, static_cast<uint32_t>(m_nextSnapshot.blocks.size()),
      m_nextSnapshot.playerRunning, static_cast<uint8_t>(complete ? 1 : 0) };
   m_publishBuf.resize(sizeof(header));
   std::memcpy(m_publishBuf.data(), &header, sizeof(header));
   for (const Wire::MemoryBlock& block : m_nextSnapshot.blocks)
   {
      const uint32_t length = static_cast<uint32_t>(block.bytes.size());
      const uint8_t* a = reinterpret_cast<const uint8_t*>(&block.address);
      const uint8_t* l = reinterpret_cast<const uint8_t*>(&length);
      m_publishBuf.insert(m_publishBuf.end(), a, a + 4);
      m_publishBuf.insert(m_publishBuf.end(), l, l + 4);
   }
   for (const Wire::MemoryBlock& block : m_nextSnapshot.blocks)
      m_publishBuf.insert(m_publishBuf.end(), block.bytes.begin(), block.bytes.end());
   m_snapshot.Write(m_publishBuf.data(), m_publishBuf.size());
}

void MemoryBridge::Answer(Request& request, const ReadFn& read, const WriteFn& write, uint64_t frame,
   bool gameRunning)
{
   if (request.kind == Request::Kind::Write)
   {
      if (!gameRunning)
      {
         request.result = Result::NoGame;
         return;
      }
      // Applied before this frame's snapshot is taken, so the next Poll shows the write.
      const uint32_t applied = write ? write(request.ops) : NO_WRITE_HANDLER;
      if (applied == NO_WRITE_HANDLER)
         request.result = Result::Unsupported;
      else if (applied == 0)
         request.result = Result::Busy;   // a handler that timed out: transient, not remembered
      else
      {
         request.writeReply = { frame, static_cast<uint16_t>(applied) };
         request.result = Result::Ok;
      }
      return;
   }

   if (request.kind == Request::Kind::Subscribe)
   {
      request.accepted.clear();
      std::vector<uint8_t> scratch;
      for (const Wire::MemoryRange& range : request.ranges)
      {
         if (!gameRunning || range.length == 0)
            continue;
         scratch.resize(range.length);
         if (read(range.address, scratch.data(), range.length) == range.length)
            request.accepted.push_back(range);
      }
      request.result = Result::Ok;
      return;
   }

   if (!gameRunning)
   {
      request.result = Result::NoGame;
      return;
   }

   Wire::ReadDirectReply& reply = request.reply;
   reply = { };
   reply.frame = frame;
   reply.address = request.address;
   reply.bytes.resize(request.length);
   std::vector<uint8_t> second(Wire::READ_CHUNK_BYTES);
   uint32_t total = 0;
   for (uint32_t offset = 0; offset < request.length; offset += Wire::READ_CHUNK_BYTES)
   {
      const uint32_t want = std::min(Wire::READ_CHUNK_BYTES, request.length - offset);
      const uint32_t first = read(request.address + offset, reply.bytes.data() + offset, want);
      const uint32_t again = read(request.address + offset, second.data(), want);
      // Readable only as far as both reads got.
      const uint32_t got = std::min({ first, again, want });
      if (got == 0)
         break;
      if (again != first || std::memcmp(reply.bytes.data() + offset, second.data(), got) != 0)
         reply.unstableOffsets.push_back(offset);
      total = offset + got;
      if (got < want)
         break;
   }
   reply.bytes.resize(total);
   request.result = total == 0 ? Result::OutOfRange : Result::Ok;
}

void MemoryBridge::Complete(Queue& requests)
{
   for (const std::shared_ptr<Request>& request : requests)
   {
      // Recorded even for a request the worker stopped waiting on: the host still answered.
      if (request->kind == Request::Kind::Write && request->result == Result::Unsupported)
         m_writesUnsupported = true;
      if (!request->abandoned && request->kind == Request::Kind::Subscribe
         && request->subscriptionId > m_subscriptionId)
      {
         m_ranges = request->accepted;
         m_subscriptionId = request->subscriptionId;
      }
      request->done = true;
   }
}

bool MemoryBridge::TakeSnapshot(const ReadFn& read, const std::vector<Wire::MemoryRange>& ranges, uint64_t frame,
   bool playerRunning, bool gameRunning)
{
   m_nextSnapshot.frame = frame;
   m_nextSnapshot.playerRunning = static_cast<uint8_t>(playerRunning);
   if (ranges.empty() || !gameRunning)
   {
      m_nextSnapshot.blocks.clear();
      return false;
   }

   m_nextSnapshot.blocks.resize(ranges.size());
   for (size_t i = 0; i < ranges.size(); i++)
   {
      Wire::MemoryBlock& block = m_nextSnapshot.blocks[i];
      block.address = ranges[i].address;
      block.bytes.resize(ranges[i].length);
      // A range that read in full at Subscribe but not now is no snapshot at all:
      // Poll must carry the set exactly, so it answers busy until it reads again;
      // a daemon that keeps seeing busy re-subscribes, which drops the range.
      if (read(block.address, block.bytes.data(), ranges[i].length) != ranges[i].length)
         return false;
   }
   return true;
}

}
