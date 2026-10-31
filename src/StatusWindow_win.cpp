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
// The ring's pulse in this many opacity steps: about 10 redraws a second while it pulses, where
// every byte value would redraw on every tick.
constexpr int RING_STEPS = 16;
constexpr wchar_t CLASS_NAME[] = L"ScorbitStatusLight";

HWND window = nullptr;
ATOM windowClass = 0;
HWINEVENTHOOK foregroundHook = nullptr;
bool inFront = false;
HWND lastFront = nullptr;  // VPX's window in front when the light was last raised

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

// Kept between frames and remade only when the frame's size changes.
HDC memory = nullptr;
HBITMAP bitmap = nullptr;
HGDIOBJ previousBitmap = nullptr;
void* bits = nullptr;
int bitsSize = 0;

void ReleaseSurface()
{
   if (memory != nullptr && previousBitmap != nullptr)
      SelectObject(memory, previousBitmap);
   if (bitmap != nullptr)
      DeleteObject(bitmap);
   if (memory != nullptr)
      DeleteDC(memory);
   memory = nullptr;
   bitmap = nullptr;
   previousBitmap = nullptr;
   bits = nullptr;
   bitsSize = 0;
}

bool Surface(HDC screen, int size)
{
   if (memory != nullptr && bitsSize == size)
      return true;
   ReleaseSurface();
   BITMAPINFO info { };
   info.bmiHeader.biSize = sizeof(info.bmiHeader);
   info.bmiHeader.biWidth = size;
   info.bmiHeader.biHeight = -size; // top-down rows, as the frame holds them
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;
   memory = CreateCompatibleDC(screen);
   bitmap = memory != nullptr ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
   if (bitmap == nullptr || bits == nullptr)
   {
      ReleaseSurface();
      return false;
   }
   previousBitmap = SelectObject(memory, bitmap);
   bitsSize = size;
   return true;
}

// GetDpiForWindow is Windows 10 1607 and later; imported by name so older Windows still loads the plugin.
UINT WindowDpi(HWND hwnd)
{
   using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
   static const auto getDpiForWindow
      = reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
   if (getDpiForWindow != nullptr)
      if (const UINT dpi = getDpiForWindow(hwnd); dpi != 0)
         return dpi;
   const HDC screen = GetDC(nullptr);
   const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSX) : 0;
   if (screen != nullptr)
      ReleaseDC(nullptr, screen);
   return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

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
   const double scale = WindowDpi(playfield) / 96.0;
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
   LightPhase phase = PhaseAt(shown, seconds, flashSeconds);
   if (shown.ring == LightRing::Rainbow)
      phase.ring = static_cast<uint8_t>(std::lround(std::round(phase.ring * (RING_STEPS - 1) / 255.0) * 255.0 / (RING_STEPS - 1)));

   const int size = std::max(1, static_cast<int>(std::lround(LightGeometry::DIAMETER * scale)));
   const int inset = static_cast<int>(std::lround(INSET_PT * scale));
   const RECT& area = monitor.rcMonitor;
   POINT at;
   at.x = (shownCorner == 1 || shownCorner == 3) ? area.left + inset : area.right - inset - size;
   at.y = (shownCorner == 3 || shownCorner == 4) ? area.bottom - inset - size : area.top + inset;

   if (!force && drawn && phase == drawnPhase && scale == drawnScale && at.x == drawnAt.x && at.y == drawnAt.y && shownAlpha == drawnAlpha)
      return;
   Rasterize(shown, phase, scale, frame);

   const HDC screen = GetDC(nullptr);
   if (screen == nullptr)
      return;
   if (Surface(screen, frame.size))
   {
      // Premultiplied 0xAARRGGBB is the byte order a 32-bit DIB holds: B, G, R, A.
      std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
      GdiFlush();
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
   }
   ReleaseDC(nullptr, screen);
}

// Shown only while VPX is the application in front, so it never sits over another one.
void FollowForeground()
{
   const HWND front = PlayfieldWindow();
   if (front == nullptr)
   {
      if (inFront)
         ShowWindow(window, SW_HIDE);
      inFront = false;
      lastFront = nullptr;
      return;
   }
   const bool appearing = !inFront;
   inFront = true;
   if (appearing)
      Draw(true); // the new frame, in place, before it shows: never the last one where it was
   // Raised again whenever VPX brings another window forward, which may itself be topmost
   // (a fullscreen playfield, a backglass).
   if (appearing || front != lastFront)
      SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (appearing ? SWP_SHOWWINDOW : 0));
   lastFront = front;
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
   ReleaseSurface();
   inFront = false;
   lastFront = nullptr;
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
