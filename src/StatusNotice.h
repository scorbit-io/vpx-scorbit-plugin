// license:GPLv3+

#pragma once

// Status indication (scorbitd SPEC-0002 REQ-6.7, 6.11): what the socket worker knows of
// the daemon's Status and of the session, handed to the API thread, and turned there into
// a notification on transitions only. The plugin's own indicator surface is still open;
// until it exists, the notification is the only thing the user sees.

#include "WireProtocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace Scorbit
{

// The whole state the API thread shows, so the latest one record is enough.
struct StatusRecord
{
   bool inSession = false;    // between HelloAck and the end of that session
   bool haveStatus = false;   // a Status arrived in this session
   Wire::Status status;
   std::string reason;        // why there is no session, when the daemon said why
   std::chrono::steady_clock::time_point since { }; // when there stopped being a session
};

// Single slot, worker to API thread. The worker exchanges in a new record and deletes the
// one it displaced; the API thread exchanges in null and owns what it took, so neither side
// ever reads a record the other can still reach. An overwritten record lasted under a frame,
// which is shorter than anyone could read it.
class StatusLatch final
{
public:
   StatusLatch() = default;
   ~StatusLatch() { delete m_slot.exchange(nullptr); }
   StatusLatch(const StatusLatch&) = delete;
   StatusLatch& operator=(const StatusLatch&) = delete;

   void Post(const StatusRecord& record) { delete m_slot.exchange(new StatusRecord(record), std::memory_order_acq_rel); }
   std::unique_ptr<StatusRecord> Take() { return std::unique_ptr<StatusRecord>(m_slot.exchange(nullptr, std::memory_order_acq_rel)); }

private:
   std::atomic<StatusRecord*> m_slot { nullptr };
};

// API thread. Decides, once per rendered frame, whether a notification is due.
class StatusNotifier final
{
public:
   using Clock = std::chrono::steady_clock;

   // Without a session, the amber notice waits this long after the session went (REQ-6.11).
   static constexpr std::chrono::seconds NO_SESSION_AFTER { 5 };

   // start counts as the moment there stopped being a session, for a plugin that never had one.
   explicit StatusNotifier(Clock::time_point start);

   // taken is what the latch held, if anything. Returns the text to post, or empty.
   std::string Frame(std::unique_ptr<StatusRecord> taken, bool showStatus, Clock::time_point now);

   // A new table starts: it shows the current state once, even if unchanged.
   void NewTable() { m_posted.clear(); }

   // "Scorbit: tracking, ok", plus " - <support text>" when there is one.
   static std::string Describe(const Wire::Status& status);
   static std::string DescribeNoSession(const std::string& reason);

private:
   StatusRecord m_current;
   std::string m_posted; // last text posted; cleared while nothing may show, so the state reposts after
};

}
