// license:GPLv3+

#pragma once

// Status indication (scorbitd SPEC-0002 REQ-6.7, 6.11): what the socket worker knows of
// the daemon's Status and of the session, handed to the API thread, and turned there into
// a notification on transitions, repeated while a [!] state lasts (REQ-6.3, 6.9). No
// plugin-API surface can carry REQ-6.6's indicator yet (SB-5996), so the notification is
// the interim primary surface.

#include "Latch.h"
#include "WireProtocol.h"

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

// Worker to API thread. An overwritten record lasted under a frame, which is shorter than
// anyone could read it.
using StatusLatch = Latch<StatusRecord>;

// API thread. Decides, once per rendered frame, whether a notification is due.
class StatusNotifier final
{
public:
   using Clock = std::chrono::steady_clock;

   // Without a session, the amber notice waits this long after the session went (REQ-6.11).
   static constexpr std::chrono::seconds NO_SESSION_AFTER { 5 };
   // A [!] state still holding is posted again this long after it last was (REQ-6.3).
   static constexpr std::chrono::seconds REFRESH_EVERY { 60 };

   // start counts as the moment there stopped being a session, for a plugin that never had one.
   explicit StatusNotifier(Clock::time_point start);

   // taken is what the latch held, if anything. Returns the text to post, or empty.
   std::string Frame(std::unique_ptr<StatusRecord> taken, bool showStatus, Clock::time_point now);

   // A new table starts: it shows the current state once, even if unchanged.
   void NewTable() { m_posted.clear(); }

   // "[OK] Scorbit: tracking, ok", plus " - <support text>" when there is one. The leading
   // marker is the shape distinction REQ-6.5 asks for, so no state is told by colour alone.
   static std::string Describe(const Wire::Status& status);
   // [OK] working, [!] expected to work and not (REQ-6.4), [?] a value this build does not know.
   static std::string Marker(const Wire::Status& status);
   static std::string DescribeNoSession(const std::string& reason);

private:
   StatusRecord m_current;
   std::string m_posted; // last text posted; cleared while nothing may show, so the state reposts after
   Clock::time_point m_postedAt { };
};

}
