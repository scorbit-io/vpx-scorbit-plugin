// license:GPLv3+

// Drives MemoryBridge the way the plugin does: requests from one thread, Service
// from another standing in for VPX's API thread, over a fake main CPU memory with
// an unreadable hole (like WPC's I/O window) and a region whose two reads disagree
// (like pinmame #642's intermittent zeros).

#include "MemoryBridge.h"

#include "Check.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace Scorbit;
using Result = MemoryBridge::Result;

namespace
{

constexpr uint32_t HOLE_START = 0x3000;
constexpr uint32_t HOLE_END = 0x4000;
constexpr uint32_t FLAKY_START = 0x9000;
constexpr uint32_t FLAKY_END = 0x9100;
constexpr int BUDGET_MS = 50;

class FakeMemory
{
public:
   FakeMemory() : m_bytes(0x10000)
   {
      for (size_t i = 0; i < m_bytes.size(); i++)
         m_bytes[i] = static_cast<uint8_t>(i * 7 + 3);
   }

   // Like stock PinmameReadMainCPUMemory: stops at the first byte it cannot read.
   uint32_t Read(uint32_t address, uint8_t* out, uint32_t size)
   {
      m_toggle++;
      uint32_t n = 0;
      for (; n < size; n++)
      {
         const uint32_t a = address + n;
         if (a >= m_bytes.size() || (a >= HOLE_START && a < HOLE_END))
            break;
         out[n] = (a >= FLAKY_START && a < FLAKY_END && (m_toggle & 1)) ? 0 : m_bytes[a];
      }
      return n;
   }

   uint8_t At(uint32_t address) const { return m_bytes[address]; }

private:
   std::vector<uint8_t> m_bytes;
   uint32_t m_toggle = 0;
};

// Calls Service every millisecond until stopped, like OnPrepareFrame would.
class ApiThread
{
public:
   ApiThread(MemoryBridge& bridge, FakeMemory& memory, bool gameRunning = true)
      : m_ready((bridge.SetGameRunning(gameRunning), true)) // as SetRomId would, before any frame
      , m_thread([this, &bridge, &memory, gameRunning]
           {
              const MemoryBridge::ReadFn read = [&memory](uint32_t a, uint8_t* o, uint32_t s) { return memory.Read(a, o, s); };
              while (m_running)
              {
                 bridge.Service(read, ++m_frame, true, gameRunning);
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
           })
   {
   }

   ~ApiThread()
   {
      m_running = false;
      m_thread.join();
   }

private:
   bool m_ready;
   std::atomic<bool> m_running { true };
   std::atomic<uint64_t> m_frame { 0 };
   std::thread m_thread;
};

std::atomic<bool> g_running { true };

// Spins until the bridge holds n queued requests, so ordering tests do not rely on sleeps.
bool WaitQueued(MemoryBridge& bridge, size_t n)
{
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (bridge.QueuedForTest() != n)
   {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return true;
}

void TestSubscribeKeepsReadableRangesInOrder()
{
   MemoryBridge bridge;
   FakeMemory memory;
   ApiThread api(bridge, memory);

   std::vector<Wire::MemoryRange> accepted;
   // The second range runs into the hole, so it is dropped rather than echoed.
   const std::vector<Wire::MemoryRange> asked { { 0x1c93, 4 }, { 0x2ff0, 0x20 }, { 0x0100, 8 } };
   CHECK(bridge.Subscribe(asked, accepted, BUDGET_MS, g_running) == Result::Ok);
   CHECK(accepted == (std::vector<Wire::MemoryRange> { { 0x1c93, 4 }, { 0x0100, 8 } }));

   Wire::PollReply poll;
   CHECK(bridge.Poll(poll) == Result::Ok);
   CHECK(poll.blocks.size() == 2);
   if (poll.blocks.size() == 2)
   {
      CHECK(poll.blocks[0].address == 0x1c93 && poll.blocks[0].bytes.size() == 4);
      CHECK(poll.blocks[1].address == 0x0100 && poll.blocks[1].bytes.size() == 8);
      CHECK(poll.blocks[0].bytes[0] == memory.At(0x1c93));
      CHECK(poll.blocks[1].bytes[7] == memory.At(0x0107));
   }
   CHECK(poll.frame != 0);
   CHECK(poll.playerRunning == 1);

   // An empty list clears the set; Poll then carries no ranges.
   CHECK(bridge.Subscribe({ }, accepted, BUDGET_MS, g_running) == Result::Ok);
   CHECK(accepted.empty());
   CHECK(bridge.Poll(poll) == Result::Ok);
   CHECK(poll.blocks.empty());
}

void TestSubscribeLimits()
{
   MemoryBridge bridge;
   std::vector<Wire::MemoryRange> accepted;

   const std::vector<Wire::MemoryRange> tooMany(Wire::MAX_SUBSCRIBE_RANGES + 1, Wire::MemoryRange { 0, 1 });
   CHECK(bridge.Subscribe(tooMany, accepted, BUDGET_MS, g_running) == Result::TooLarge);
   const std::vector<Wire::MemoryRange> tooBig { { 0, 0x2000 }, { 0x5000, 0x2001 } };
   CHECK(bridge.Subscribe(tooBig, accepted, BUDGET_MS, g_running) == Result::TooLarge);
   const std::vector<Wire::MemoryRange> wraps { { 0xFFFFFFF0u, 0x20 } };
   CHECK(bridge.Subscribe(wraps, accepted, BUDGET_MS, g_running) == Result::OutOfRange);
}

void TestPollWithoutAGame()
{
   MemoryBridge bridge;
   FakeMemory memory;
   ApiThread api(bridge, memory, false);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));

   Wire::PollReply poll;
   CHECK(bridge.Poll(poll) == Result::NoGame);
   Wire::ReadDirectReply read;
   CHECK(bridge.ReadDirect(0x0100, 4, read, BUDGET_MS, g_running) == Result::NoGame);
}

void TestClearWinsOverAQueuedSubscribe()
{
   // No API thread yet: the Subscribe waits in the queue while a Declare clears.
   MemoryBridge bridge;
   FakeMemory memory;
   const MemoryBridge::ReadFn read = [&memory](uint32_t a, uint8_t* o, uint32_t s) { return memory.Read(a, o, s); };
   bridge.SetGameRunning(true);

   std::vector<Wire::MemoryRange> accepted;
   auto pending = std::async(std::launch::async, [&]
      { return bridge.Subscribe({ { 0x0100, 8 } }, accepted, 2000, g_running); });
   CHECK(WaitQueued(bridge, 1));
   bridge.Clear();
   bridge.Service(read, 2, true, true);

   CHECK(pending.get() == Result::Ok);
   Wire::PollReply poll;
   CHECK(bridge.Poll(poll) == Result::Ok);
   CHECK_MSG(poll.blocks.empty(), "a set subscribed before the Declare came back after it");
}

void TestReadDirect()
{
   MemoryBridge bridge;
   FakeMemory memory;
   ApiThread api(bridge, memory);

   Wire::ReadDirectReply read;
   CHECK(bridge.ReadDirect(0x8000, 0x8000, read, BUDGET_MS, g_running) == Result::Ok);
   CHECK(read.address == 0x8000 && read.bytes.size() == 0x8000);
   CHECK(read.frame != 0);
   // Only the flaky 256-byte chunk disagreed between its two passes.
   CHECK(read.unstableOffsets == (std::vector<uint32_t> { FLAKY_START - 0x8000 }));
   CHECK(read.bytes[0x10] == memory.At(0x8010));

   // Short where memory stops being readable: 0x2f00 has 0x100 bytes before the hole.
   CHECK(bridge.ReadDirect(0x2f00, 0x200, read, BUDGET_MS, g_running) == Result::Ok);
   CHECK(read.bytes.size() == 0x100);
   CHECK(read.unstableOffsets.empty());

   // Nothing readable at all is out of range, not an empty success.
   CHECK(bridge.ReadDirect(HOLE_START, 0x10, read, BUDGET_MS, g_running) == Result::OutOfRange);
   CHECK(bridge.ReadDirect(0, Wire::MAX_READ_DIRECT_BYTES + 1, read, BUDGET_MS, g_running) == Result::TooLarge);
   CHECK(bridge.ReadDirect(0x100, 0, read, BUDGET_MS, g_running) == Result::OutOfRange);
}

void TestBusyWhenTheApiThreadIsSilent()
{
   MemoryBridge bridge;
   FakeMemory memory;
   const MemoryBridge::ReadFn read = [&memory](uint32_t a, uint8_t* o, uint32_t s) { return memory.Read(a, o, s); };
   bridge.SetGameRunning(true);
   bridge.Service(read, 1, true, true);

   // A game is running but VPX stopped rendering: nothing services the queue.
   Wire::ReadDirectReply out;
   const auto t0 = std::chrono::steady_clock::now();
   CHECK(bridge.ReadDirect(0x0100, 4, out, BUDGET_MS, g_running) == Result::Busy);
   const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
   CHECK_MSG(ms < 100, "busy took " + std::to_string(ms) + " ms, past the daemon's 100 ms");

   // A stopping worker gives up at once rather than waiting out the budget.
   std::atomic<bool> stopping { false };
   const auto t1 = std::chrono::steady_clock::now();
   CHECK(bridge.ReadDirect(0x0100, 4, out, 1000, stopping) == Result::Busy);
   const auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
   CHECK_MSG(ms1 < 50, "a stopping worker waited " + std::to_string(ms1) + " ms");

   // The abandoned requests left the queue: a later Service has nothing to answer.
   CHECK(bridge.QueuedForTest() == 0);
   bridge.Service(read, 2, true, true);
   Wire::PollReply poll;
   CHECK(bridge.Poll(poll) == Result::Ok);
}

// The table chooser: the player closed, so no frame renders and nothing calls
// Service. The worker must learn that from SetGameRunning and answer at once,
// not busy, which the daemon would retry forever.
void TestNoGameWhileNothingRenders()
{
   MemoryBridge bridge;
   FakeMemory memory;
   const MemoryBridge::ReadFn read = [&memory](uint32_t a, uint8_t* o, uint32_t s) { return memory.Read(a, o, s); };
   bridge.SetGameRunning(true);
   bridge.Service(read, 1, true, true);
   std::vector<Wire::MemoryRange> accepted;
   std::atomic<bool> running { true };
   auto pending = std::async(std::launch::async, [&]
      { return bridge.Subscribe({ { 0x0100, 8 } }, accepted, 2000, running); });
   CHECK(WaitQueued(bridge, 1));
   bridge.Service(read, 2, true, true);
   CHECK(pending.get() == Result::Ok);
   Wire::PollReply poll;
   CHECK(bridge.Poll(poll) == Result::Ok && poll.blocks.size() == 1);

   bridge.SetGameRunning(false); // OnGameEnd; no Service follows

   const auto t0 = std::chrono::steady_clock::now();
   CHECK(bridge.Poll(poll) == Result::NoGame);
   Wire::ReadDirectReply read2;
   CHECK(bridge.ReadDirect(0x0100, 4, read2, BUDGET_MS, g_running) == Result::NoGame);
   CHECK(bridge.Subscribe({ { 0x0100, 8 } }, accepted, BUDGET_MS, g_running) == Result::Ok);
   CHECK(accepted.empty());
   const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
   CHECK_MSG(ms < 20, "with no game the answers took " + std::to_string(ms) + " ms; they should not wait");

   // A new game on the same ROM: serving resumes as soon as frames render again.
   bridge.SetGameRunning(true);
   ApiThread api(bridge, memory);
   CHECK(bridge.Subscribe({ { 0x0100, 8 } }, accepted, BUDGET_MS, g_running) == Result::Ok);
   CHECK(accepted.size() == 1);
}

void TestSnapshotRefreshesEachFrame()
{
   MemoryBridge bridge;
   FakeMemory memory;
   ApiThread api(bridge, memory);

   std::vector<Wire::MemoryRange> accepted;
   CHECK(bridge.Subscribe({ { 0x0100, 8 } }, accepted, BUDGET_MS, g_running) == Result::Ok);
   Wire::PollReply first, later;
   CHECK(bridge.Poll(first) == Result::Ok);
   std::this_thread::sleep_for(std::chrono::milliseconds(20));
   CHECK(bridge.Poll(later) == Result::Ok);
   CHECK_MSG(later.frame > first.frame, "the snapshot was not refreshed per frame");
}

}

int main()
{
   TestSubscribeKeepsReadableRangesInOrder();
   TestSubscribeLimits();
   TestPollWithoutAGame();
   TestClearWinsOverAQueuedSubscribe();
   TestReadDirect();
   TestBusyWhenTheApiThreadIsSilent();
   TestSnapshotRefreshesEachFrame();
   TestNoGameWhileNothingRenders();
   return ScorbitTest::Summary("memory_bridge_test");
}
