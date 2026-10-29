// license:GPLv3+

#pragma once

// The committed Game batches waiting for the socket worker (SB-5990). The API thread pushes,
// the worker takes everything at once. While no daemon is connected nothing is taken, so the
// queue is bounded in ops, and when full it keeps the newest games: the oldest whole game goes
// first, and only when the current game alone fills it are its later batches refused until
// the next game_start.

#include "SocketWorker.h"
#include "WireProtocol.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace Scorbit
{

// The ops the plugin holds for the worker at most, the daemon's own pre-resolution queue cap.
inline constexpr size_t MAX_QUEUED_GAME_OPS = 16384;

class GameQueue final
{
public:
   explicit GameQueue(size_t maxOps) : m_maxOps(maxOps) { }

   // Any thread. Appends the posts in order; true when anything was dropped doing so.
   bool Push(std::vector<GamePost>&& posts)
   {
      bool dropped = false;
      std::lock_guard lock(m_mutex);
      for (GamePost& post : posts)
      {
         const bool starts = !post.batch.ops.empty() && post.batch.ops.front().kind == Wire::GAME_START;
         if (m_refusing && !starts)
         {
            dropped = true;
            continue;
         }
         const size_t ops = post.batch.ops.size();
         while (m_ops + ops > m_maxOps && EvictOldestGame(starts))
            dropped = true;
         if (m_ops + ops > m_maxOps)
         {
            m_refusing = true;
            dropped = true;
            continue;
         }
         m_refusing = false;
         m_ops += ops;
         m_queue.push_back(std::move(post));
      }
      return dropped;
   }

   // Any thread. Moves everything queued to out, oldest first.
   void TakeAll(std::deque<GamePost>& out)
   {
      std::lock_guard lock(m_mutex);
      for (GamePost& post : m_queue)
         out.push_back(std::move(post));
      m_queue.clear();
      m_ops = 0;
   }

   size_t QueuedOps()
   {
      std::lock_guard lock(m_mutex);
      return m_ops;
   }

private:
   // Under m_mutex. Drops the oldest game: everything before the next queued game_start. With
   // none queued, everything is the current game, which only a new game may evict.
   bool EvictOldestGame(bool incomingStarts)
   {
      if (m_queue.empty())
         return false;
      size_t next = 1;
      while (next < m_queue.size() && m_queue[next].batch.ops.front().kind != Wire::GAME_START)
         next++;
      if (next == m_queue.size() && !incomingStarts)
         return false;
      for (size_t i = 0; i < next; i++)
         m_ops -= m_queue[i].batch.ops.size();
      m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(next));
      return true;
   }

   const size_t m_maxOps;
   std::mutex m_mutex;
   std::deque<GamePost> m_queue;
   size_t m_ops = 0;
   bool m_refusing = false; // the current game outgrew the queue; refused until the next start
};

}
