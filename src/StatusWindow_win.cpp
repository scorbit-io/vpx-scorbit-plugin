// license:GPLv3+

#include "StatusWindow.h"
#include "StatusLightRaster.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// A layered window that never activates and takes no input: VPX pauses whenever its playfield
// loses focus. It is created, drawn and destroyed on the plugin API thread, which is VPX's main
// thread and pumps this window's messages. Every handle is released before the plugin unloads,
// since the window procedure and the hook callback are code in this DLL.
namespace Scorbit
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT TIMER_MS = 25;      // under the shortest dark phase of a dot (100 ms)
constexpr double INSET_PT = 16;
constexpr wchar_t CLASS_NAME[] = L"ScorbitStatusLight";

HWND window = nullptr;
ATOM windowClass = 0;
HWINEVENTHOOK foregroundHook = nullptr;
bool inFront = false;

LightView shown;
Clock::time_point shownSince;
Clock::time_point flashAt;
bool flashing = false;
int shownCorner = 0;
BYTE shownAlpha = 255;

// What was last handed to UpdateLayeredWindow, to draw again only on a change.
bool drawn = false;
LightPhase drawnPhase;
double drawnScale = 0;
POINT drawnAt { };
BYTE drawnAlpha = 0;
LightFrame frame;

HINSTANCE ThisModule()
{
   HMODULE module = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ThisModule), &module);
   return module;
}

// VPX plays only while its playfield holds keyboard focus, so while VPX is in front, the
// foreground window is the playfield.
HWND PlayfieldWindow()
{
   const HWND front = GetForegroundWindow();
   DWORD pid = 0;
   if (front == nullptr || front == window || GetWindowThreadProcessId(front, &pid) == 0 || pid != GetCurrentProcessId())
      return nullptr;
   return front;
}

void Draw(bool force)
{
   if (window == nullptr || !inFront)
      return;
   const HWND playfield = PlayfieldWindow();
   if (playfield == nullptr)
      return;
   const UINT dpi = GetDpiForWindow(playfield);
   const double scale = (dpi == 0 ? 96 : dpi) / 96.0;
   MONITORINFO monitor { };
   monitor.cbSize = sizeof(monitor);
   if (!GetMonitorInfoW(MonitorFromWindow(playfield, MONITOR_DEFAULTTOPRIMARY), &monitor))
      return;

   const auto now = Clock::now();
   const double seconds = std::chrono::duration<double>(now - shownSince).count();
   double flashSeconds = -1;
   if (flashing)
   {
      flashSeconds = std::chrono::duration<double>(now - flashAt).count();
      if (flashSeconds >= LightGeometry::FLASH_S)
      {
         flashing = false;
         flashSeconds = -1;
      }
   }
   const LightPhase phase = PhaseAt(shown, seconds, flashSeconds);

   const int size = std::max(1, static_cast<int>(std::lround(LightGeometry::DIAMETER * scale)));
   const int inset = static_cast<int>(std::lround(INSET_PT * scale));
   const RECT& area = monitor.rcMonitor;
   POINT at;
   at.x = (shownCorner == 1 || shownCorner == 3) ? area.left + inset : area.right - inset - size;
   at.y = (shownCorner == 3 || shownCorner == 4) ? area.bottom - inset - size : area.top + inset;

   if (!force && drawn && phase == drawnPhase && scale == drawnScale && at.x == drawnAt.x && at.y == drawnAt.y && shownAlpha == drawnAlpha)
      return;
   Rasterize(shown, phase, scale, frame);

   BITMAPINFO info { };
   info.bmiHeader.biSize = sizeof(info.bmiHeader);
   info.bmiHeader.biWidth = frame.size;
   info.bmiHeader.biHeight = -frame.size; // top-down rows, as the frame holds them
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;
   void* bits = nullptr;
   const HDC screen = GetDC(nullptr);
   const HDC memory = CreateCompatibleDC(screen);
   const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
   if (memory != nullptr && bitmap != nullptr && bits != nullptr)
   {
      // Premultiplied 0xAARRGGBB is the byte order a 32-bit DIB holds: B, G, R, A.
      std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
      const HGDIOBJ previous = SelectObject(memory, bitmap);
      SIZE extent { frame.size, frame.size };
      POINT origin { 0, 0 };
      BLENDFUNCTION blend { AC_SRC_OVER, 0, shownAlpha, AC_SRC_ALPHA };
      if (UpdateLayeredWindow(window, screen, &at, &extent, memory, &origin, 0, &blend, ULW_ALPHA))
      {
         drawn = true;
         drawnPhase = phase;
         drawnScale = scale;
         drawnAt = at;
         drawnAlpha = shownAlpha;
      }
      SelectObject(memory, previous);
   }
   if (bitmap != nullptr)
      DeleteObject(bitmap);
   if (memory != nullptr)
      DeleteDC(memory);
   ReleaseDC(nullptr, screen);
}

// Shown only while VPX is the application in front, so it never sits over another one.
void FollowForeground()
{
   const bool front = PlayfieldWindow() != nullptr;
   if (front == inFront)
      return;
   inFront = front;
   if (front)
   {
      // Raised again in case VPX's own topmost fullscreen window went above it.
      SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      Draw(true);
   }
   else
      ShowWindow(window, SW_HIDE);
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
   if (window != nullptr)
      FollowForeground();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
   switch (message)
   {
   case WM_TIMER:
      if (wParam == TIMER_ID)
         Draw(false);
      return 0;
   case WM_NCHITTEST:
      return HTTRANSPARENT;
   case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
   default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
   }
}

void Hide()
{
   if (foregroundHook != nullptr)
      UnhookWinEvent(foregroundHook);
   foregroundHook = nullptr;
   if (window != nullptr)
   {
      KillTimer(window, TIMER_ID);
      DestroyWindow(window);
   }
   window = nullptr;
   if (windowClass != 0)
      UnregisterClassW(CLASS_NAME, ThisModule());
   windowClass = 0;
   inFront = false;
   drawn = false;
   flashing = false;
}

bool Show()
{
   const HINSTANCE module = ThisModule();
   WNDCLASSEXW cls { };
   cls.cbSize = sizeof(cls);
   cls.lpfnWndProc = WindowProc;
   cls.hInstance = module;
   cls.lpszClassName = CLASS_NAME;
   windowClass = RegisterClassExW(&cls);
   if (windowClass == 0)
      return false;
   window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, CLASS_NAME, L"",
      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
   if (window == nullptr || SetTimer(window, TIMER_ID, TIMER_MS, nullptr) == 0)
   {
      Hide();
      return false;
   }
   // Out of context: delivered through this thread's message queue, no DLL injected anywhere.
   foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
   return true;
}

}

bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash)
{
   if (corner < 1 || corner > 4 || !view.visible)
   {
      if (window != nullptr)
         Hide();
      shownCorner = 0;
      return false;
   }
   const bool appeared = window == nullptr;
   const auto now = Clock::now();
   if (appeared)
   {
      if (!Show())
         return false;
      shown = view;
      shownSince = now;
   }
   else if (!(view == shown))
   {
      shown = view;
      shownSince = now;
   }
   if (flash)
   {
      flashing = true;
      flashAt = now;
   }
   shownCorner = corner;
   shownAlpha = static_cast<BYTE>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255));
   FollowForeground();
   Draw(false);
   return appeared;
}

}
