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

}

StatusNotifier::StatusNotifier(Clock::time_point start)
{
   m_current.since = start;
}

std::string StatusNotifier::Describe(const Wire::Status& status)
{
   std::string text = "Scorbit: " + SessionName(status.sessionState);
   if (status.sessionState != Wire::SESSION_HIDDEN)
      text += ", " + SystemName(status.systemState);
   if (!status.supportText.empty())
      text += " - " + status.supportText;
   return text;
}

std::string StatusNotifier::DescribeNoSession(const std::string& reason)
{
   return "Scorbit: no data, no connection to the Scorbit daemon" + (reason.empty() ? std::string() : " - " + reason);
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
