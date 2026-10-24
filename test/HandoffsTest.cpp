// license:GPLv3+

// The lock-free handoffs (docs/slice4-internals.md): the seqlock never hands a reader a torn
// value, the latch keeps only the latest record, and the overlay cache keeps the API
// thread's ops in arrival order without ever waiting on the other writer.

#include "Latch.h"
#include "OverlayCache.h"
#include "SeqLock.h"

#include "Check.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Scorbit;

namespace
{

void TestSeqLockRoundTrip()
{
   SeqLock<64> lock;
   std::vector<uint8_t> buf;
   size_t size = 0;
   auto out = [&] { return std::vector<uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(size)); };
   CHECK_MSG(!lock.Read(buf, size), "nothing written yet");
   CHECK(lock.Sequence() == 0);

   const uint8_t odd[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
   CHECK(lock.Write(odd, sizeof(odd)));
   const uint64_t first = lock.Sequence();
   CHECK(first != 0 && (first & 1) == 0);
   CHECK(lock.Read(buf, size) && out() == std::vector<uint8_t>(odd, odd + sizeof(odd)));

   CHECK(lock.Write(odd, 0));
   CHECK(lock.Sequence() != first);
   CHECK(lock.Read(buf, size) && size == 0);

   std::vector<uint8_t> big(65, 7);
   CHECK_MSG(!lock.Write(big.data(), big.size()), "over capacity publishes nothing");
   CHECK(lock.Read(buf, size) && size == 0);
}

// One writer, several readers, sizes that change with every write. Each write fills every
// byte with the same value, so a torn copy shows up as two values in one buffer.
void TestSeqLockNeverHandsOutATornValue()
{
   constexpr size_t MAX = 4096;
   SeqLock<MAX> lock;
   std::atomic<bool> stop { false };
   std::atomic<int> torn { 0 };
   std::atomic<long> reads { 0 };

   std::thread writer([&]
   {
      std::vector<uint8_t> buf(MAX);
      for (uint32_t k = 1; !stop; k++)
      {
         const size_t size = 8 + (k * 37) % (MAX - 8);
         std::memset(buf.data(), static_cast<int>(k & 0xFF), size);
         lock.Write(buf.data(), size);
      }
   });
   std::vector<std::thread> readers;
   for (int r = 0; r < 3; r++)
      readers.emplace_back([&]
      {
         std::vector<uint8_t> out;
         size_t size = 0;
         while (!stop)
         {
            if (!lock.Read(out, size))
               continue;
            reads++;
            for (size_t i = 0; i < size; i++)
               if (out[i] != out[0])
               {
                  torn++;
                  break;
               }
         }
      });

   std::this_thread::sleep_for(std::chrono::milliseconds(300));
   stop = true;
   writer.join();
   for (std::thread& t : readers)
      t.join();
   CHECK_MSG(torn == 0, std::to_string(torn.load()) + " torn copies handed out");
   CHECK_MSG(reads > 0, "no read ever completed");
}

// At a frame-like pace a reader that lands on a write waits it out: a read that gives up is
// what once made a live display look gone and re-declared it as 0x0.
void TestSeqLockReadersRarelyGiveUpAtFrameRate()
{
   constexpr size_t MAX = 16 * 1024;
   SeqLock<MAX> lock;
   std::atomic<bool> stop { false };
   std::atomic<long> reads { 0 }, failed { 0 };
   std::vector<uint8_t> first(MAX, 1);
   lock.Write(first.data(), first.size());

   std::thread writer([&]
   {
      std::vector<uint8_t> buf(MAX);
      for (uint32_t k = 2; !stop; k++)
      {
         std::memset(buf.data(), static_cast<int>(k & 0xFF), buf.size());
         lock.Write(buf.data(), buf.size());
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
   });
   std::vector<std::thread> readers;
   for (int r = 0; r < 2; r++)
      readers.emplace_back([&]
      {
         std::vector<uint8_t> out;
         size_t size = 0;
         while (!stop)
         {
            reads++;
            if (!lock.Read(out, size))
               failed++;
         }
      });
   std::this_thread::sleep_for(std::chrono::milliseconds(300));
   stop = true;
   writer.join();
   for (std::thread& t : readers)
      t.join();
   CHECK_MSG(failed * 1000 <= reads, std::to_string(failed.load()) + " of " + std::to_string(reads.load()) + " reads gave up");
}

void TestLatchKeepsTheLatest()
{
   Latch<std::string> latch;
   CHECK(latch.Take() == nullptr);
   latch.Post("a");
   latch.Post("b");
   const std::unique_ptr<std::string> taken = latch.Take();
   CHECK(taken != nullptr && *taken == "b");
   CHECK(latch.Take() == nullptr);
   latch.Post("left for the destructor");
}

// Holds the cache's writer lock from another thread, as the worker would: try_lock on a
// mutex the calling thread already owns is undefined.
class WorkerHolds final
{
public:
   explicit WorkerHolds(Overlay::Cache& cache)
      : m_thread([this, &cache]
           {
              auto lock = cache.HoldWritersForTest();
              m_held = true;
              while (!m_release)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
           })
   {
      while (!m_held)
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   ~WorkerHolds() { Release(); }
   void Release()
   {
      m_release = true;
      if (m_thread.joinable())
         m_thread.join();
   }

private:
   std::atomic<bool> m_held { false };
   std::atomic<bool> m_release { false };
   std::thread m_thread;
};

std::vector<uint8_t> Upload(uint8_t w, uint8_t h, uint8_t fill)
{
   std::vector<uint8_t> p { w, h };
   p.resize(2 + static_cast<size_t>(w) * h, fill);
   return p;
}

Overlay::Record g_scratch;

void TestCacheReadsOnlyWhenChanged()
{
   Overlay::Cache cache;
   Overlay::Record record;
   uint64_t seq = 0;
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK(record.width == 0 && !record.Visible(Overlay::Cache::Clock::now()));
   CHECK_MSG(!cache.ReadIfChanged(seq, record, g_scratch), "an unchanged record is not copied again");

   const std::vector<uint8_t> p = Upload(10, 10, 0x03);
   CHECK(cache.Store(p.data(), p.size()) == Overlay::State::Upload::Stored);
   cache.Control(true, 0);
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK(record.width == 10 && record.height == 10 && record.pixels[99] == 0x03);
   CHECK(record.Visible(Overlay::Cache::Clock::now() + std::chrono::hours(1)));

   cache.Clear();
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK(record.width == 0 && !record.Visible(Overlay::Cache::Clock::now()));
}

void TestCacheTimerCountsFromArrival()
{
   Overlay::Cache cache;
   const std::vector<uint8_t> p = Upload(10, 10, 0x03);
   const auto before = Overlay::Cache::Clock::now();
   {
      WorkerHolds held(cache);
      CHECK(cache.PostUpload(p.data(), p.size()));
      CHECK(cache.PostControl(true, 100));
      CHECK_MSG(cache.PendingForTest() == 2, "the worker holds the writers, so both ops wait");
      cache.DrainPending();
      CHECK_MSG(cache.PendingForTest() == 2, "a drain that cannot take the lock leaves the queue");
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(150));
   cache.DrainPending();
   CHECK(cache.PendingForTest() == 0);
   Overlay::Record record;
   uint64_t seq = 0;
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK_MSG(record.width == 10, "the queued upload applied");
   CHECK_MSG(!record.Visible(Overlay::Cache::Clock::now()), "100 ms from arrival have passed, though it applied just now");
   CHECK(record.Visible(before + std::chrono::milliseconds(50)));
}

void TestCacheKeepsArrivalOrder()
{
   Overlay::Cache cache;
   const std::vector<uint8_t> first = Upload(10, 10, 0x01), second = Upload(12, 10, 0x02);
   {
      WorkerHolds held(cache);
      CHECK(cache.PostUpload(first.data(), first.size()));
   }
   // The lock is free now, but the queue is not empty: this must queue behind the first.
   CHECK(cache.PostUpload(second.data(), second.size()));
   CHECK_MSG(cache.PendingForTest() == 2, "a later op never overtakes a queued one");
   cache.DrainPending();
   Overlay::Record record;
   uint64_t seq = 0;
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK_MSG(record.width == 12 && record.pixels[0] == 0x02, "the second upload is the one that stays");
}

void TestCacheRefusesWhenFullOrInvalid()
{
   Overlay::Cache cache;
   const std::vector<uint8_t> p = Upload(10, 10, 0x01);
   WorkerHolds held(cache);
   for (size_t i = 0; i < Overlay::Cache::PENDING_LIMIT; i++)
      CHECK(cache.PostControl(true, 0));
   CHECK_MSG(!cache.PostControl(false, 0), "a full queue refuses");
   CHECK(cache.PendingForTest() == Overlay::Cache::PENDING_LIMIT);

   const std::vector<uint8_t> small = Upload(9, 9, 0x01);
   CHECK_MSG(!cache.PostUpload(small.data(), small.size()), "an out of range upload is refused, not queued");
   CHECK_MSG(!cache.PostUpload(p.data(), p.size() - 1), "a short upload is refused");
}

// A queued upload keeps only its pixels: trailing bytes are ignored, so they are not copied.
void TestCacheQueuesAnUploadWithoutItsTrailer()
{
   Overlay::Cache cache;
   std::vector<uint8_t> p = Upload(10, 10, 0x05);
   p.resize(p.size() + 1024 * 1024, 0xEE);
   {
      WorkerHolds held(cache);
      CHECK(cache.PostUpload(p.data(), p.size()));
      CHECK(cache.PendingForTest() == 1);
   }
   cache.DrainPending();
   Overlay::Record record;
   uint64_t seq = 0;
   CHECK(cache.ReadIfChanged(seq, record, g_scratch));
   CHECK_MSG(record.width == 10 && record.height == 10 && record.pixels[99] == 0x05, "the bitmap, without its trailer");
}

// The worker writes through the cache while a reader copies; every copy is a whole record.
void TestCacheReaderNeverSeesAMixedRecord()
{
   Overlay::Cache cache;
   std::atomic<bool> stop { false };
   std::atomic<int> mixed { 0 };
   std::thread writer([&]
   {
      for (uint8_t k = 0; !stop; k++)
      {
         const std::vector<uint8_t> p = Upload(static_cast<uint8_t>(10 + k % 100), 64, k);
         cache.Store(p.data(), p.size());
      }
   });
   Overlay::Record record;
   uint64_t seq = 0;
   const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
   while (std::chrono::steady_clock::now() < end)
   {
      if (!cache.ReadIfChanged(seq, record, g_scratch) || record.width == 0)
         continue;
      const size_t n = static_cast<size_t>(record.width) * record.height;
      for (size_t i = 1; i < n; i++)
         if (record.pixels[i] != record.pixels[0])
         {
            mixed++;
            break;
         }
   }
   stop = true;
   writer.join();
   CHECK_MSG(mixed == 0, std::to_string(mixed.load()) + " mixed records");
}

}

int main()
{
   TestSeqLockRoundTrip();
   TestSeqLockNeverHandsOutATornValue();
   TestSeqLockReadersRarelyGiveUpAtFrameRate();
   TestLatchKeepsTheLatest();
   TestCacheReadsOnlyWhenChanged();
   TestCacheTimerCountsFromArrival();
   TestCacheKeepsArrivalOrder();
   TestCacheRefusesWhenFullOrInvalid();
   TestCacheQueuesAnUploadWithoutItsTrailer();
   TestCacheReaderNeverSeesAMixedRecord();
   return ScorbitTest::Summary("handoffs_test");
}
