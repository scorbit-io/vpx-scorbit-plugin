// license:GPLv3+

#pragma once

// A single-slot handoff between two threads, holding only the latest value
// (docs/slice4-internals.md). The producer exchanges in a new immutable record and deletes
// the one it displaced; the consumer exchanges in null and owns what it took, so neither
// side ever reads a record the other can still reach, and neither waits on the other.
// Bounded at one record by construction. Used for the Status and Declare handoffs.

#include <atomic>
#include <memory>

namespace Scorbit
{

template <class T>
class Latch final
{
public:
   Latch() = default;
   ~Latch() { delete m_slot.exchange(nullptr); }
   Latch(const Latch&) = delete;
   Latch& operator=(const Latch&) = delete;

   void Post(const T& value) { delete m_slot.exchange(new T(value), std::memory_order_acq_rel); }
   std::unique_ptr<T> Take() { return std::unique_ptr<T>(m_slot.exchange(nullptr, std::memory_order_acq_rel)); }

private:
   std::atomic<T*> m_slot { nullptr };
};

}
