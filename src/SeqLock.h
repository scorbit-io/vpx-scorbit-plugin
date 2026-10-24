// license:GPLv3+

#pragma once

// A seqlock over one fixed-capacity buffer: one writer at a time publishes, any number of
// readers copy, and no reader ever waits on the writer (docs/slice4-internals.md).
//
// The buffer is stored as atomic words, written and read relaxed between the sequence
// counter's release and acquire. A reader that overlaps a write therefore copies a torn
// value without a data race, sees the counter move, and discards it. A plain memcpy under
// a seqlock would be undefined behaviour. Writers must be serialised by the caller.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace Scorbit
{

template <size_t MaxBytes>
class SeqLock final
{
public:
   static constexpr size_t CAPACITY = MaxBytes;
   // A reader that sees more torn copies than this in a row reports nothing for that read.
   static constexpr unsigned MAX_RETRIES = 3;
   // A write in progress is waited out, not counted as a torn copy: a copy takes the writer
   // about a microsecond, so attempts spent at once would all fail. Bounded all the same.
   static constexpr unsigned MAX_YIELDS = 1000;

   SeqLock() = default;
   SeqLock(const SeqLock&) = delete;
   SeqLock& operator=(const SeqLock&) = delete;

   // Writer. Publishes nothing and returns false when size is over CAPACITY.
   bool Write(const void* data, size_t size)
   {
      if (size > MaxBytes)
         return false;
      const uint64_t seq = m_seq.load(std::memory_order_relaxed);
      m_seq.store(seq + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      m_size.store(size, std::memory_order_relaxed);
      const uint8_t* bytes = static_cast<const uint8_t*>(data);
      for (size_t i = 0, n = (size + 7) / 8; i < n; i++)
      {
         uint64_t word = 0;
         std::memcpy(&word, bytes + i * 8, std::min<size_t>(8, size - i * 8));
         m_words[i].store(word, std::memory_order_relaxed);
      }
      m_seq.store(seq + 2, std::memory_order_release);
      return true;
   }

   // Any thread. Copies the latest complete value into out (at least CAPACITY bytes) and
   // sets size. False when nothing has been written, or the writer tore every copy.
   bool Read(void* out, size_t& size, uint64_t* sequence = nullptr) const
   {
      uint8_t* dst = static_cast<uint8_t*>(out);
      unsigned yields = 0;
      for (unsigned attempt = 0; attempt <= MAX_RETRIES; attempt++)
      {
         uint64_t before = m_seq.load(std::memory_order_acquire);
         while ((before & 1) && yields++ < MAX_YIELDS)
         {
            std::this_thread::yield();
            before = m_seq.load(std::memory_order_acquire);
         }
         if (before == 0)
            return false;
         if (before & 1)
            continue;
         const size_t n = static_cast<size_t>(m_size.load(std::memory_order_relaxed));
         if (n > MaxBytes)
            continue;
         for (size_t i = 0, words = (n + 7) / 8; i < words; i++)
         {
            const uint64_t word = m_words[i].load(std::memory_order_relaxed);
            std::memcpy(dst + i * 8, &word, std::min<size_t>(8, n - i * 8));
         }
         std::atomic_thread_fence(std::memory_order_acquire);
         if (m_seq.load(std::memory_order_relaxed) == before)
         {
            size = n;
            if (sequence != nullptr)
               *sequence = before;
            return true;
         }
      }
      return false;
   }

   // Into a buffer kept at CAPACITY, so it is allocated and zeroed once, not per read.
   bool Read(std::vector<uint8_t>& buffer, size_t& size, uint64_t* sequence = nullptr) const
   {
      if (buffer.size() != MaxBytes)
         buffer.resize(MaxBytes);
      return Read(buffer.data(), size, sequence);
   }

   // Any thread. Zero until the first write, then even between writes and different after
   // each, so a reader can skip copying a value it already holds.
   uint64_t Sequence() const { return m_seq.load(std::memory_order_acquire); }

private:
   std::atomic<uint64_t> m_seq { 0 };
   std::atomic<uint64_t> m_size { 0 };
   std::array<std::atomic<uint64_t>, (MaxBytes + 7) / 8> m_words { };
};

}
