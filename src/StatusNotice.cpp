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

std::string StatusNotifier::DescribeEarlier(const std::string& described)
{
   const std::string head = "] Scorbit: ";
   const size_t at = described.find(head);
   if (at == std::string::npos)
      return "During the game: " + described;
   return described.substr(0, at) + "] Scorbit, during the game: " + described.substr(at + head.size());
}

void StatusNotifier::NewTable()
{
   m_posted.clear();
   TableEnded();
}

void StatusNotifier::TableEnded()
{
   m_inGame = false;
   m_authoredGame = false;
   m_lostInGame = false;
   m_afterGame = false;
   // The worker ends the old table's game on its next Declare; a frame before that must not.
   m_current.history.romGame = false;
   m_current.history.romGameEndsAt = { };
}

void StatusNotifier::GameStarted()
{
   m_authoredGame = true;
   m_authoredGames++;
}

void StatusNotifier::GameEnded()
{
   m_authoredGame = false;
}

std::string StatusNotifier::Want(Clock::time_point now) const
{
   // A session still waiting for its first Status, or a loss younger than the grace, keeps
   // what is posted: a quick reconnect into the same state is not a transition.
   if (m_current.inSession)
      return m_current.haveStatus ? Describe(m_current.status) : std::string();
   if (now - m_current.since >= NO_SESSION_AFTER)
      return DescribeNoSession(m_current.reason);
   return { };
}

bool StatusNotifier::RomGameInProgress(Clock::time_point now) const
{
   const StatusHistory& h = m_current.history;
   if (!h.romGame)
      return false;
   if (h.romGameEndsAt != Clock::time_point { } && now >= h.romGameEndsAt)
      return false;
   // The worker drops the game itself on a reconnect this late; this is the same bound while
   // there is still no connection.
   if (!m_current.inSession && now - m_current.since >= ROM_GAME_LOST_AFTER)
      return false;
   return true;
}

NoticeAction StatusNotifier::Frame(std::unique_ptr<StatusRecord> taken, bool showStatus, int notices, bool authored, Clock::time_point now)
{
   if (taken)
      m_current = std::move(*taken);
   const StatusHistory& h = m_current.history;

   // The game, by the table's kind. A ROM game that began (and maybe ended) since the last
   // frame shows as a moved start count; an authored one as a GameStarted call.
   const bool quiet = notices == NOTICES_OUTSIDE_GAME;
   const uint32_t starts = authored ? m_authoredGames : h.romGames;
   if (authored != m_authoredLast)
   {
      m_authoredLast = authored;
      m_seenStarts = starts;
   }
   const bool startedSince = quiet && starts != m_seenStarts;
   m_seenStarts = starts;
   const bool inGame = quiet && (authored ? m_authoredGame : RomGameInProgress(now));
   const bool wasInGame = m_inGame;
   m_inGame = inGame;
   const bool started = !wasInGame && (inGame || startedSince);
   const bool ended = (wasInGame || startedSince) && !inGame;
   if (started)
   {
      // The worker marks a ROM game's start on the Status that starts it; a frame is too
      // coarse, as a table-load warning and the tracking can arrive between two.
      m_warningsAtStart = authored ? m_seenWarnings : h.warningsAtGameStart;
      m_lostInGame = false;
      m_afterGame = false;
   }
   m_seenWarnings = h.warnings;

   const bool hidden = m_current.inSession && m_current.haveStatus && m_current.status.sessionState == Wire::SESSION_HIDDEN;
   if (!showStatus || notices == NOTICES_NEVER || hidden)
   {
      m_posted.clear();
      return { };
   }

   NoticeAction action;
   const std::string want = Want(now);
   if (inGame)
   {
      if (started && !m_posted.empty() && now - m_postedAt < NOTICE_LIFETIME)
         action = { NoticeAction::Kind::Expire, m_posted };
      // The plugin's own [!], which no daemon Status counts: reported at the end if it recovers.
      if (!m_current.inSession && want.starts_with("[!]"))
      {
         m_lostInGame = true;
         m_lostText = want;
      }
      return action;
   }

   if (ended)
   {
      // One notice if anything went wrong during the game: the current state when it is
      // still [!], else the [!] that came up meanwhile. The current state then counts as
      // shown either way, so a change back to [OK] does not follow with a second notice.
      if (want.starts_with("[!]"))
         action = { NoticeAction::Kind::Post, want };
      else if (h.warnings != m_warningsAtStart && !h.lastWarning.empty())
         action = { NoticeAction::Kind::Post, DescribeEarlier(h.lastWarning) };
      else if (m_lostInGame)
         action = { NoticeAction::Kind::Post, DescribeEarlier(m_lostText) };
      // With nothing current to acknowledge, the summary itself is what a game may take down.
      m_posted = want.empty() ? action.text : want;
      // Only a post is on screen to take down: a game starting right after one that ended
      // quietly must not expire a notice that was never there.
      if (action.kind == NoticeAction::Kind::Post)
         m_postedAt = now;
      m_lostInGame = false;
      m_afterGame = true;
      return action;
   }

   if (want.empty())
      return { };
   // After a script's game_end the daemon still says tracking until it has taken the batch,
   // then no game: that settling is not news. Any [!] is.
   if (m_afterGame && want.starts_with("[OK]") && m_posted.starts_with("[OK]"))
   {
      // One settling change is consumed; what changes after it is news again.
      if (want != m_posted)
      {
         m_posted = want;
         m_afterGame = false;
      }
      return { };
   }
   if (want == m_posted && !(want.starts_with("[!]") && now - m_postedAt >= REFRESH_EVERY))
      return { };
   m_posted = want;
   m_postedAt = now;
   m_afterGame = false;
   return { NoticeAction::Kind::Post, want };
}

}
