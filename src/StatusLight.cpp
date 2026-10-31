// license:GPLv3+

#include "StatusLight.h"

namespace Scorbit
{

namespace
{

LightDot Blink(LightRgb rgb, uint16_t onMs, uint16_t offMs)
{
   return { true, rgb, onMs, offMs };
}

bool SystemDot(uint8_t state, LightDot& dot)
{
   LightRgb rgb;
   switch (state)
   {
   case Wire::SYSTEM_OK: rgb = Light::GREEN; break;
   case Wire::SYSTEM_UNPAIRED: rgb = Light::RED; break;
   case Wire::SYSTEM_OFFLINE: rgb = Light::ORANGE; break;
   case Wire::SYSTEM_DEGRADED: rgb = Light::PURPLE; break;
   default: return false;
   }
   dot = Blink(rgb, Light::SYSTEM_ON_MS, Light::SYSTEM_OFF_MS);
   return true;
}

// The device's own precedence: no credentials first, then the game state.
bool SessionDot(uint8_t state, bool unauthorized, LightDot& dot)
{
   LightRgb rgb;
   if (unauthorized)
      rgb = Light::RED;
   else
      switch (state)
      {
      case Wire::SESSION_NONE: rgb = Light::BLUE; break;
      case Wire::SESSION_TRACKING: rgb = Light::GREEN; break;
      case Wire::SESSION_NO_DATA: rgb = Light::AMBER; break;
      default: return false;
      }
   dot = Blink(rgb, Light::SESSION_ON_MS, Light::SESSION_OFF_MS);
   return true;
}

}

LightView LightFor(const StatusRecord& record, StatusNotifier::Clock::time_point now, const LightView& shown)
{
   LightView view;
   view.visible = true;
   if (!record.inSession)
   {
      if (now - record.since < StatusNotifier::NO_SESSION_AFTER)
         return shown;
      // Nothing drives the LEDs without a daemon; the ring says why, as the pad does.
      view.ring = LightRing::Solid;
      view.ringRgb = Light::BLUE;
      view.glyph = LightGlyph::Problem;
      return view;
   }
   if (!record.haveStatus)
      return shown;

   const Wire::Status& s = record.status;
   if (s.sessionState == Wire::SESSION_HIDDEN)
      return { };
   // A 1.0 or 1.1 daemon sends no authentication state, and its session state stands.
   const bool unauthorized = s.hasAuthStatus && (s.authStatus == Wire::AUTH_NOT_AUTHENTICATED || s.authStatus == Wire::AUTH_FAILED);
   const bool knownSystem = SystemDot(s.systemState, view.system);
   const bool knownSession = SessionDot(s.sessionState, unauthorized, view.session);

   if (!knownSystem || !knownSession)
      view.glyph = LightGlyph::Unknown;
   else if (unauthorized || s.systemState != Wire::SYSTEM_OK || s.sessionState == Wire::SESSION_NO_DATA)
      view.glyph = LightGlyph::Problem;

   if (unauthorized || s.systemState == Wire::SYSTEM_UNPAIRED)
   {
      view.ring = LightRing::Solid;
      view.ringRgb = Light::RED;
   }
   // Dark while a game is scored, so the ring never competes with the table.
   else if (view.glyph == LightGlyph::None && s.sessionState == Wire::SESSION_NONE)
      view.ring = LightRing::Rainbow;
   return view;
}

bool ClaimWatch::Update(const Ids& ids)
{
   bool claimed = false;
   if (m_known)
      for (size_t i = 0; i < SLOTS; i++)
         claimed = claimed || (!ids[i].empty() && ids[i] != m_ids[i]);
   m_ids = ids;
   m_known = true;
   return claimed;
}

}
