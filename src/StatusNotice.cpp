// license:GPLv3+

#include "StatusNotice.h"

namespace Scorbit
{

namespace
{

std::string Unknown(uint8_t value)
{
   return "unknown (" + std::to_string(value) + ')';
}

std::string SessionName(uint8_t value)
{
   switch (value)
   {
   case Wire::SESSION_NONE: return "no game";
   case Wire::SESSION_TRACKING: return "tracking";
   case Wire::SESSION_NO_DATA: return "no data";
   case Wire::SESSION_HIDDEN: return "hidden";
   default: return Unknown(value);
   }
}

std::string SystemName(uint8_t value)
{
   switch (value)
   {
   case Wire::SYSTEM_OK: return "ok";
   case Wire::SYSTEM_UNPAIRED: return "not paired";
   case Wire::SYSTEM_OFFLINE: return "offline";
   case Wire::SYSTEM_DEGRADED: return "degraded";
   default: return Unknown(value);
   }
}

bool KnownSession(uint8_t value)
{
   return value == Wire::SESSION_NONE || value == Wire::SESSION_TRACKING || value == Wire::SESSION_NO_DATA
      || value == Wire::SESSION_HIDDEN;
}

bool KnownSystem(uint8_t value)
{
   return value <= Wire::SYSTEM_DEGRADED;
}

}

// ASCII, because the notification font has no symbol glyphs we can rely on (SB-6003).
std::string StatusNotifier::Marker(const Wire::Status& status)
{
   if (!KnownSession(status.sessionState) || !KnownSystem(status.systemState))
      return "[?]";
   if (status.sessionState == Wire::SESSION_NO_DATA || status.systemState != Wire::SYSTEM_OK)
      return "[!]";
   return "[OK]";
}

StatusNotifier::StatusNotifier(Clock::time_point start)
{
   m_current.since = start;
}

std::string StatusNotifier::Describe(const Wire::Status& status)
{
   // Hidden is never posted, so it carries no marker.
   if (status.sessionState == Wire::SESSION_HIDDEN)
      return "Scorbit: hidden" + (status.supportText.empty() ? std::string() : " - " + status.supportText);
   std::string text = Marker(status) + " Scorbit: " + SessionName(status.sessionState) + ", "
      + SystemName(status.systemState);
   if (!status.supportText.empty())
      text += " - " + status.supportText;
   return text;
}

std::string StatusNotifier::DescribeNoSession(const std::string& reason)
{
   return "[!] Scorbit: no data, no connection to the Scorbit daemon" + (reason.empty() ? std::string() : " - " + reason);
}

std::string StatusNotifier::Frame(std::unique_ptr<StatusRecord> taken, bool showStatus, Clock::time_point now)
{
   if (taken)
      m_current = std::move(*taken);

   const bool hidden = m_current.inSession && m_current.haveStatus && m_current.status.sessionState == Wire::SESSION_HIDDEN;
   if (!showStatus || hidden)
   {
      m_posted.clear();
      return { };
   }

   // A session still waiting for its first Status, or a loss younger than the grace, keeps
   // what is posted: a quick reconnect into the same state is not a transition.
   std::string want;
   if (m_current.inSession)
   {
      if (m_current.haveStatus)
         want = Describe(m_current.status);
   }
   else if (now - m_current.since >= NO_SESSION_AFTER)
      want = DescribeNoSession(m_current.reason);

   if (want.empty() || want == m_posted)
      return { };
   m_posted = want;
   return want;
}

}
