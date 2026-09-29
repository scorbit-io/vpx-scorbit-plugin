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
      Clear();
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
   std::lock_guard lock(m_mutex);
   if (m_ranges.empty())
   {
      out = { };
      out.frame = m_snapshot.frame;
      out.playerRunning = m_snapshot.playerRunning;
      return Result::Ok;
   }
   if (!m_haveSnapshot || m_snapshotId != m_subscriptionId)
      return Result::Busy;
   out = m_snapshot;
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

void MemoryBridge::Clear()
{
   std::lock_guard lock(m_mutex);
   m_ranges.clear();
   // A fresh id, so a Subscribe already queued from before this point cannot
   // install its set once the API thread gets to it.
   m_subscriptionId = m_nextSubscriptionId++;
   m_haveSnapshot = false;
}

MemoryBridge::Result MemoryBridge::Submit(const std::shared_ptr<Request>& request, int budgetMs,
   const std::atomic<bool>& running)
{
   {
      std::lock_guard lock(m_mutex);
      if (m_queue.size() >= QUEUE_LIMIT)
         return Result::Busy;
      m_queue.push_back(request);
   }

   // Woken by Service; the short slice is only so a stopping worker notices. A
   // plain sleep would add up to 16 ms per request on Windows' default timer.
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
   std::unique_lock lock(m_mutex);
   while (!request->done && running && std::chrono::steady_clock::now() < deadline)
      m_answered.wait_for(lock, std::chrono::milliseconds(5));

   // Decided under the lock the API thread drains with: either it finished the
   // request, or it never will, because the request leaves the queue here.
   if (request->done)
      return request->result;
   m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), request), m_queue.end());
   return Result::Busy;
}

void MemoryBridge::SetGameRunning(bool running)
{
   std::lock_guard lock(m_mutex);
   m_gameRunning = running;
   if (!running)
   {
      m_haveSnapshot = false;
      m_snapshot.playerRunning = 0;
   }
}

size_t MemoryBridge::QueuedForTest()
{
   std::lock_guard lock(m_mutex);
   return m_queue.size();
}

void MemoryBridge::Service(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning)
{
   {
      std::unique_lock lock(m_mutex, std::try_to_lock);
      if (!lock.owns_lock())
         return;

      m_gameRunning = gameRunning;
      for (const std::shared_ptr<Request>& request : m_queue)
      {
         Answer(*request, read, frame, gameRunning);
         request->done = true;
      }
      m_queue.clear();
      TakeSnapshot(read, frame, playerRunning, gameRunning);
   }
   // Never blocks: waking a waiting worker is all this does.
   m_answered.notify_all();
}

void MemoryBridge::Answer(Request& request, const ReadFn& read, uint64_t frame, bool gameRunning)
{
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
      if (request.subscriptionId > m_subscriptionId)
      {
         m_ranges = request.accepted;
         m_subscriptionId = request.subscriptionId;
         m_haveSnapshot = false;
      }
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
      const uint32_t got = std::min(first, want);
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

void MemoryBridge::TakeSnapshot(const ReadFn& read, uint64_t frame, bool playerRunning, bool gameRunning)
{
   m_snapshot.frame = frame;
   m_snapshot.playerRunning = static_cast<uint8_t>(playerRunning);
   if (m_ranges.empty() || !gameRunning)
   {
      m_snapshot.blocks.clear();
      m_haveSnapshot = false;
      return;
   }

   m_snapshot.blocks.resize(m_ranges.size());
   for (size_t i = 0; i < m_ranges.size(); i++)
   {
      Wire::MemoryBlock& block = m_snapshot.blocks[i];
      block.address = m_ranges[i].address;
      block.bytes.resize(m_ranges[i].length);
      // A range that read in full at Subscribe but not now is no snapshot at all:
      // Poll must carry the set exactly, so it answers busy until it reads again;
      // a daemon that keeps seeing busy re-subscribes, which drops the range.
      if (read(block.address, block.bytes.data(), m_ranges[i].length) != m_ranges[i].length)
      {
         m_haveSnapshot = false;
         return;
      }
   }
   m_snapshotId = m_subscriptionId;
   m_haveSnapshot = true;
}

}
