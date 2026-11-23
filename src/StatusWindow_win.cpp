// license:GPLv3+

#include "StatusWindow.h"
#include "StatusLightRaster.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
// The caption beside the light while the cursor hovers over it (docs/status-notices.md, "Hover").
constexpr double CAPTION_GAP_PT = 8;
constexpr double CAPTION_MAX_WIDTH_PT = 360;
constexpr double CAPTION_MAX_HEIGHT_PT = 240;
constexpr double CAPTION_PAD_X_PT = 8;
constexpr double CAPTION_PAD_Y_PT = 5;
constexpr double CAPTION_CORNER_PT = 6;
constexpr double CAPTION_FONT_PT = 12;

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
LightView drawnView;
LightPhase drawnPhase;
double drawnScale = 0;
POINT drawnAt { };
BYTE drawnAlpha = 0;
LightFrame frame;

// The caption: a second window of the same kind, drawn again only when its text or scale changes
// and moved with the light.
HWND captionWindow = nullptr;
HoverTracker hover;
std::string drawnCaption;
double drawnCaptionScale = 0;
BYTE drawnCaptionAlpha = 0;
SIZE captionSize { };
bool captionShown = false;

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

   if (!force && drawn && shown == drawnView && phase == drawnPhase && scale == drawnScale && at.x == drawnAt.x && at.y == drawnAt.y && shownAlpha == drawnAlpha)
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
         drawnView = shown;
         drawnPhase = phase;
         drawnScale = scale;
         drawnAt = at;
         drawnAlpha = shownAlpha;
      }
   }
   ReleaseDC(nullptr, screen);
}

void HideCaption()
{
   if (captionWindow != nullptr && captionShown)
      ShowWindow(captionWindow, SW_HIDE);
   captionShown = false;
}

// UTF-8, as the wire carries it; bytes that are not UTF-8 are read in the system code page
// rather than dropped.
std::wstring Widen(const std::string& text)
{
   const int length = static_cast<int>(text.size());
   UINT page = CP_UTF8;
   int n = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
   if (n <= 0)
   {
      page = CP_ACP;
      n = MultiByteToWideChar(page, 0, text.data(), length, nullptr, 0);
   }
   if (n <= 0)
      return L"";
   std::wstring out(static_cast<size_t>(n), L'\0');
   MultiByteToWideChar(page, 0, text.data(), length, out.data(), n);
   return out;
}

// The caption's pixels: GDI draws the text in white on black, which gives each pixel its text
// coverage but no alpha, so the alpha is set here, over black at the light's background opacity,
// with rounded corners. Grayscale antialiasing, since ClearType's colour fringes have no alpha.
bool DrawCaption(const std::string& text, double scale, BYTE alpha)
{
   const std::wstring wide = Widen(text);
   const HDC screen = GetDC(nullptr);
   if (screen == nullptr)
      return false;
   bool ok = false;
   const int dpi = static_cast<int>(std::lround(96 * scale));
   HFONT font = CreateFontW(-MulDiv(static_cast<int>(CAPTION_FONT_PT), dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
   const int padX = static_cast<int>(std::lround(CAPTION_PAD_X_PT * scale));
   const int padY = static_cast<int>(std::lround(CAPTION_PAD_Y_PT * scale));
   const int corner = static_cast<int>(std::lround(CAPTION_CORNER_PT * scale));
   HDC dc = CreateCompatibleDC(screen);
   HBITMAP dib = nullptr;
   void* pixels = nullptr;
   if (dc != nullptr && font != nullptr)
   {
      const HGDIOBJ previousFont = SelectObject(dc, font);
      // Measured within the caption's bounds: a single word wider than them, or a text of
      // many lines, is clipped at the edge rather than growing the window past the monitor.
      const LONG maxWidth = static_cast<LONG>(std::lround(CAPTION_MAX_WIDTH_PT * scale));
      const LONG maxHeight = static_cast<LONG>(std::lround(CAPTION_MAX_HEIGHT_PT * scale));
      RECT measure { 0, 0, maxWidth, 0 };
      DrawTextW(dc, wide.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_TOP);
      const int width = std::clamp<int>(measure.right - measure.left, 1, maxWidth) + 2 * padX;
      const int height = std::clamp<int>(measure.bottom - measure.top, 1, maxHeight) + 2 * padY;
      BITMAPINFO info { };
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = width;
      info.bmiHeader.biHeight = -height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
      if (dib != nullptr && pixels != nullptr)
      {
         const HGDIOBJ previousBitmap = SelectObject(dc, dib);
         std::memset(pixels, 0, static_cast<size_t>(width) * height * 4);
         SetBkMode(dc, TRANSPARENT);
         SetTextColor(dc, RGB(255, 255, 255));
         RECT box { padX, padY, width - padX, height - padY };
         DrawTextW(dc, wide.c_str(), -1, &box, DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_TOP);
         GdiFlush();
         uint32_t* px = static_cast<uint32_t*>(pixels);
         for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
               uint32_t& p = px[static_cast<size_t>(y) * width + x];
               // Outside a corner's quarter circle: nothing.
               const int cx = x < corner ? corner - 1 - x : x >= width - corner ? x - (width - corner) : -1;
               const int cy = y < corner ? corner - 1 - y : y >= height - corner ? y - (height - corner) : -1;
               if (cx >= 0 && cy >= 0 && (cx + 1) * (cx + 1) + (cy + 1) * (cy + 1) > corner * corner)
               {
                  p = 0;
                  continue;
               }
               const uint32_t coverage = ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3;
               const uint32_t a = coverage + LightGeometry::BACKGROUND_ALPHA * (255 - coverage) / 255;
               p = (a << 24) | (coverage << 16) | (coverage << 8) | coverage; // premultiplied white over black
            }
         SIZE extent { width, height };
         POINT origin { 0, 0 };
         BLENDFUNCTION blend { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
         // Drawn at the origin; placed by SetWindowPos afterwards, which a layered window keeps.
         if (UpdateLayeredWindow(captionWindow, screen, nullptr, &extent, dc, &origin, 0, &blend, ULW_ALPHA))
         {
            captionSize = extent;
            ok = true;
         }
         SelectObject(dc, previousBitmap);
      }
      SelectObject(dc, previousFont);
   }
   if (dib != nullptr)
      DeleteObject(dib);
   if (dc != nullptr)
      DeleteDC(dc);
   if (font != nullptr)
      DeleteObject(font);
   ReleaseDC(nullptr, screen);
   return ok;
}

// Beside the light on the side toward the monitor's centre, level with it, at its opacity.
void ShowCaption(const std::string& text)
{
   if (captionWindow == nullptr)
   {
      captionWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, CLASS_NAME, L"",
         WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, ThisModule(), nullptr);
      if (captionWindow == nullptr)
         return;
   }
   if (text != drawnCaption || drawnScale != drawnCaptionScale || shownAlpha != drawnCaptionAlpha)
   {
      if (!DrawCaption(text, drawnScale, shownAlpha))
         return;
      drawnCaption = text;
      drawnCaptionScale = drawnScale;
      drawnCaptionAlpha = shownAlpha;
   }
   MONITORINFO monitor { };
   monitor.cbSize = sizeof(monitor);
   const POINT centre { drawnAt.x + frame.size / 2, drawnAt.y + frame.size / 2 };
   if (!GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST), &monitor))
      return;
   const RECT& area = monitor.rcMonitor;
   const int gap = static_cast<int>(std::lround(CAPTION_GAP_PT * drawnScale));
   const bool lightOnRight = centre.x > (area.left + area.right) / 2;
   const int x = lightOnRight ? drawnAt.x - gap - captionSize.cx : drawnAt.x + frame.size + gap;
   // Level with the light, kept on the monitor; the bounds stay ordered even for a caption
   // taller than it.
   const int lowest = std::max<int>(area.top, area.bottom - captionSize.cy);
   const int y = std::clamp<int>(centre.y - captionSize.cy / 2, area.top, lowest);
   SetWindowPos(captionWindow, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
   captionShown = true;
}

// Shown only while VPX is the application in front, so it never sits over another one.
void FollowForeground()
{
   const HWND front = PlayfieldWindow();
   if (front == nullptr)
   {
      if (inFront)
         ShowWindow(window, SW_HIDE);
      HideCaption();
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
   case WM_CLOSE:
      // A close sent to every window of the process (taskkill, logoff) is VPX's to answer. The light
      // goes when the plugin hides it; destroyed here, the plugin would hold a dead handle.
      return 0;
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
   if (captionWindow != nullptr)
      DestroyWindow(captionWindow);
   captionWindow = nullptr;
   captionShown = false;
   drawnCaption.clear();
   drawnCaptionScale = 0;
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

bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash, const std::string& caption)
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
   // Hover: the cursor's position read with no input of our own, so the light stays
   // click-through; a hidden cursor (VPX hides it on a press) shows nothing.
   POINT cursor { };
   CURSORINFO cursorInfo { };
   cursorInfo.cbSize = sizeof(cursorInfo);
   const bool haveCursor = GetCursorPos(&cursor) != FALSE;
   const bool hidden = GetCursorInfo(&cursorInfo) != FALSE && (cursorInfo.flags & CURSOR_SHOWING) == 0;
   const bool over = haveCursor && inFront && drawn && cursor.x >= drawnAt.x && cursor.x < drawnAt.x + frame.size
      && cursor.y >= drawnAt.y && cursor.y < drawnAt.y + frame.size;
   if (hover.Tick(cursor.x, cursor.y, over, hidden, now) && !caption.empty())
      ShowCaption(caption);
   else
      HideCaption();
   return appeared;
}

// --- The status alert banner (docs/status-banner.md) ---------------------------------------
// A third window of the light's kind, centred in the playfield window a quarter of the way down,
// shown only while an alert is up and VPX is in front. Destroyed, with its class, at unload.

namespace
{

constexpr wchar_t BANNER_CLASS[] = L"ScorbitStatusBanner";
constexpr uint32_t BANNER_R = 0xFF, BANNER_G = 0xD4, BANNER_B = 0x00;  // #FFD400

HWND bannerWindow = nullptr;
HWND bannerPlayfield = nullptr;  // the playfield it was last placed over
ATOM bannerClass = 0;
bool bannerFailed = false;     // the window could not be made: VPX's notice, for the plugin's life
bool bannerUp = false;
Clock::time_point bannerUntil;
BannerText bannerText;
RECT bannerArea { };           // the playfield's client area it was laid out for, in screen pixels
double bannerScale = 0;
bool bannerDrawn = false;
bool bannerShownNow = false;

LRESULT CALLBACK BannerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
   switch (message)
   {
   case WM_NCHITTEST:
      return HTTRANSPARENT;
   case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
   case WM_ACTIVATE:
      // Nothing of ours may hold the foreground: VPX pauses when its playfield loses it. A tool that
      // focuses VPX by "main window" can pick this one (it has no owner); hand the foreground back.
      if (LOWORD(wParam) != WA_INACTIVE && bannerPlayfield != nullptr && IsWindow(bannerPlayfield))
         SetForegroundWindow(bannerPlayfield);
      return 0;
   case WM_CLOSE:
      return 0;  // as for the light: the plugin destroys it
   default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
   }
}

bool MakeBanner()
{
   if (bannerWindow != nullptr)
      return true;
   if (bannerFailed)
      return false;
   const HINSTANCE module = ThisModule();
   WNDCLASSEXW cls { };
   cls.cbSize = sizeof(cls);
   cls.lpfnWndProc = BannerProc;
   cls.hInstance = module;
   cls.lpszClassName = BANNER_CLASS;
   bannerClass = RegisterClassExW(&cls);
   if (bannerClass != 0)
      bannerWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, BANNER_CLASS, L"",
         WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
   if (bannerWindow == nullptr)
   {
      if (bannerClass != 0)
         UnregisterClassW(BANNER_CLASS, module);
      bannerClass = 0;
      bannerFailed = true;
      return false;
   }
   return true;
}

// The playfield's client area in screen pixels, or false while VPX is not in front.
bool PlayfieldArea(HWND& playfield, RECT& area)
{
   playfield = PlayfieldWindow();
   if (playfield == nullptr || playfield == bannerWindow || !GetClientRect(playfield, &area))
      return false;
   POINT topLeft { area.left, area.top };
   POINT bottomRight { area.right, area.bottom };
   ClientToScreen(playfield, &topLeft);
   ClientToScreen(playfield, &bottomRight);
   area = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
   return area.right > area.left && area.bottom > area.top;
}

// Coverage of a pixel by the icon's shape, 4x4 supersampled: a triangle pointing up, or a disc.
uint32_t ShapeCoverage(bool triangle, double x, double y, double size)
{
   int inside = 0;
   for (int sy = 0; sy < 4; sy++)
      for (int sx = 0; sx < 4; sx++)
      {
         const double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
         if (triangle)
         {
            // Apex at the top centre, base along the bottom.
            const double half = size / 2 * (py / size);
            inside += (py >= 0 && py <= size && std::abs(px - size / 2) <= half) ? 1 : 0;
         }
         else
         {
            const double dx = px - size / 2, dy = py - size / 2;
            inside += dx * dx + dy * dy <= size * size / 4 ? 1 : 0;
         }
      }
   return static_cast<uint32_t>(inside * 255 / 16);
}

// Draws the text (and measures it) in white on black into a DIB of the given size; returns its
// coverage, one byte a pixel. With measure set, only measures, into width and height.
bool TextCoverage(HDC screen, HFONT font, const std::wstring& text, UINT format, int boxWidth, int& width, int& height,
   std::vector<uint8_t>* coverage)
{
   HDC dc = CreateCompatibleDC(screen);
   if (dc == nullptr)
      return false;
   const HGDIOBJ previousFont = SelectObject(dc, font);
   bool ok = false;
   if (coverage == nullptr)
   {
      RECT measure { 0, 0, boxWidth, 0 };
      DrawTextW(dc, text.c_str(), -1, &measure, format | DT_CALCRECT);
      width = measure.right - measure.left;
      height = measure.bottom - measure.top;
      ok = true;
   }
   else
   {
      BITMAPINFO info { };
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = width;
      info.bmiHeader.biHeight = -height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void* pixels = nullptr;
      HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
      if (dib != nullptr && pixels != nullptr)
      {
         const HGDIOBJ previousBitmap = SelectObject(dc, dib);
         std::memset(pixels, 0, static_cast<size_t>(width) * height * 4);
         SetBkMode(dc, TRANSPARENT);
         SetTextColor(dc, RGB(255, 255, 255));
         RECT box { 0, 0, width, height };
         DrawTextW(dc, text.c_str(), -1, &box, format);
         GdiFlush();
         const uint32_t* px = static_cast<const uint32_t*>(pixels);
         coverage->resize(static_cast<size_t>(width) * height);
         for (size_t i = 0; i < coverage->size(); i++)
            (*coverage)[i] = static_cast<uint8_t>(((px[i] & 0xFF) + ((px[i] >> 8) & 0xFF) + ((px[i] >> 16) & 0xFF)) / 3);
         SelectObject(dc, previousBitmap);
         ok = true;
      }
      if (dib != nullptr)
         DeleteObject(dib);
   }
   SelectObject(dc, previousFont);
   DeleteDC(dc);
   return ok;
}

// The longest prefix of the text that, with an ellipsis, wraps to at most maxHeight; cut at a word
// where there is one. The text itself when it fits.
std::wstring FitBannerText(HDC screen, HFONT font, const std::wstring& text, UINT format, int boxWidth, int maxHeight)
{
   int w = 0, h = 0;
   if (!TextCoverage(screen, font, text, format, boxWidth, w, h, nullptr) || h <= maxHeight)
      return text;
   const auto cut = [&](size_t n) {
      std::wstring prefix = text.substr(0, n);
      while (!prefix.empty() && iswspace(prefix.back()))
         prefix.pop_back();
      return prefix + L"\u2026";
   };
   size_t fits = 0, over = text.size();
   while (over - fits > 1)
   {
      const size_t mid = fits + (over - fits) / 2;
      TextCoverage(screen, font, cut(mid), format, boxWidth, w, h, nullptr);
      (h <= maxHeight ? fits : over) = mid;
   }
   if (fits > 0 && IS_HIGH_SURROGATE(text[fits - 1]))
      fits--;
   const size_t space = text.find_last_of(L" \t", fits);
   if (space != std::wstring::npos && space > 0 && fits < text.size() && !iswspace(text[fits]))
      fits = space;
   return cut(fits);
}

// Lays out and draws the banner for this area (docs/status-banner.md, "Size"): 20 DIPs x DPI / 96,
// as a character height (a negative lfHeight), solid black, yellow text, the icon level with the
// first line. Grayscale antialiasing, since ClearType's colour fringes have no alpha.
bool DrawBanner(const BannerText& banner, const RECT& area, double scale)
{
   const HDC screen = GetDC(nullptr);
   if (screen == nullptr)
      return false;
   const int textPx = std::max(1, static_cast<int>(std::lround(BANNER_TEXT_HEIGHT * scale)));
   HFONT font = CreateFontW(-textPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
   HFONT glyphFont = nullptr;
   bool ok = false;
   if (font != nullptr)
   {
      TEXTMETRICW metrics { };
      HDC probe = CreateCompatibleDC(screen);
      if (probe != nullptr)
      {
         const HGDIOBJ previous = SelectObject(probe, font);
         GetTextMetricsW(probe, &metrics);
         SelectObject(probe, previous);
         DeleteDC(probe);
      }
      const int line = std::max<int>(1, metrics.tmHeight);
      const int pad = (textPx + 1) / 2;
      const int icon = line;
      const int maxText = std::max(1, static_cast<int>(BANNER_MAX_WIDTH * (area.right - area.left)) - 3 * pad - icon);
      const UINT format = DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_TOP;
      // At most three lines, the last ending in an ellipsis when the text is longer.
      const std::wstring text = FitBannerText(screen, font, Widen(banner.text), format, maxText, BANNER_MAX_LINES * line);
      int textW = 0, textH = 0;
      TextCoverage(screen, font, text, format, maxText, textW, textH, nullptr);
      textW = std::clamp(textW, 1, maxText);
      textH = std::clamp(textH, 1, BANNER_MAX_LINES * line);
      std::vector<uint8_t> textCov;
      const bool textOk = TextCoverage(screen, font, text, format, textW, textW, textH, &textCov);

      const int glyphPx = std::max(1, static_cast<int>(std::lround(icon * (banner.icon == BannerIcon::Problem ? 0.55 : 0.65))));
      glyphFont = CreateFontW(-glyphPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      const wchar_t* glyph = banner.icon == BannerIcon::Problem ? L"!" : banner.icon == BannerIcon::Unknown ? L"?" : L"i";
      std::vector<uint8_t> glyphCov;
      int glyphW = icon, glyphH = icon;
      const bool glyphOk = glyphFont != nullptr
         && TextCoverage(screen, glyphFont, glyph, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, icon, glyphW, glyphH, &glyphCov);

      const int width = pad + icon + pad + textW + pad;
      const int inner = std::max(textH, icon);
      const int height = pad + inner + pad;
      const int corner = std::max(1, pad / 2);
      const int textX = pad + icon + pad, textY = pad + (inner - textH) / 2;
      // A triangle's middle sits low; its glyph follows it.
      const int glyphShift = banner.icon == BannerIcon::Problem ? static_cast<int>(std::lround(icon * 0.14)) : 0;
      const bool triangle = banner.icon == BannerIcon::Problem;
      const uint32_t shapeR = 0xFF, shapeG = triangle ? BANNER_G : 0xFF, shapeB = triangle ? BANNER_B : 0xFF;

      BITMAPINFO info { };
      info.bmiHeader.biSize = sizeof(info.bmiHeader);
      info.bmiHeader.biWidth = width;
      info.bmiHeader.biHeight = -height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void* pixels = nullptr;
      HDC dc = CreateCompatibleDC(screen);
      HBITMAP dib = (textOk && dc != nullptr) ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
      if (dib != nullptr && pixels != nullptr)
      {
         const HGDIOBJ previousBitmap = SelectObject(dc, dib);
         uint32_t* px = static_cast<uint32_t*>(pixels);
         for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
               uint32_t& p = px[static_cast<size_t>(y) * width + x];
               const int cx = x < corner ? corner - 1 - x : x >= width - corner ? x - (width - corner) : -1;
               const int cy = y < corner ? corner - 1 - y : y >= height - corner ? y - (height - corner) : -1;
               if (cx >= 0 && cy >= 0 && (cx + 1) * (cx + 1) + (cy + 1) * (cy + 1) > corner * corner)
               {
                  p = 0;
                  continue;
               }
               uint32_t r = 0, g = 0, b = 0;  // opaque black, so each colour is already premultiplied
               const int tx = x - textX, ty = y - textY;
               if (tx >= 0 && ty >= 0 && tx < textW && ty < textH)
               {
                  const uint32_t c = textCov[static_cast<size_t>(ty) * textW + tx];
                  r = BANNER_R * c / 255;
                  g = BANNER_G * c / 255;
                  b = BANNER_B * c / 255;
               }
               const int ix = x - pad, iy = y - pad;
               if (ix >= 0 && iy >= 0 && ix < icon && iy < icon)
               {
                  const uint32_t s = ShapeCoverage(triangle, ix, iy, icon);
                  const int gy = iy - glyphShift;
                  const uint32_t k = (glyphOk && gy >= 0 && gy < glyphH && ix < glyphW) ? glyphCov[static_cast<size_t>(gy) * glyphW + ix] : 0;
                  const uint32_t lit = s * (255 - k) / 255;  // the glyph is cut out of the shape, in black
                  r = shapeR * lit / 255;
                  g = shapeG * lit / 255;
                  b = shapeB * lit / 255;
               }
               p = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
         GdiFlush();
         SIZE extent { width, height };
         POINT origin { 0, 0 };
         const int areaW = area.right - area.left, areaH = area.bottom - area.top;
         POINT at { area.left + (areaW - width) / 2, area.top + static_cast<LONG>(std::lround(BANNER_TOP * areaH)) };
         BLENDFUNCTION blend { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
         ok = UpdateLayeredWindow(bannerWindow, screen, &at, &extent, dc, &origin, 0, &blend, ULW_ALPHA) != FALSE;
         SelectObject(dc, previousBitmap);
      }
      if (dib != nullptr)
         DeleteObject(dib);
      if (dc != nullptr)
         DeleteDC(dc);
   }
   if (glyphFont != nullptr)
      DeleteObject(glyphFont);
   if (font != nullptr)
      DeleteObject(font);
   ReleaseDC(nullptr, screen);
   return ok;
}

void HideBannerWindow()
{
   if (bannerWindow != nullptr && bannerShownNow)
      ShowWindow(bannerWindow, SW_HIDE);
   bannerShownNow = false;
}

// Lays out again when the playfield moved, resized or changed DPI, and raises it while VPX is in
// front; hides it otherwise. False only when drawing failed, not while it waits for VPX.
bool FollowBanner()
{
   HWND playfield = nullptr;
   RECT area { };
   if (!bannerUp || bannerWindow == nullptr || !PlayfieldArea(playfield, area))
   {
      HideBannerWindow();
      return true;
   }
   bannerPlayfield = playfield;
   const double scale = WindowDpi(playfield) / 96.0;
   if (!bannerDrawn || !EqualRect(&area, &bannerArea) || scale != bannerScale)
   {
      if (!DrawBanner(bannerText, area, scale))
      {
         HideBannerWindow();
         return false;
      }
      bannerDrawn = true;
      bannerArea = area;
      bannerScale = scale;
   }
   SetWindowPos(bannerWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
   bannerShownNow = true;
   return true;
}

std::string Lower(std::string s)
{
   for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return s;
}

}

bool StatusBannerSupported()
{
   // Built here, so a banner that can't be made never drops a state VPX's notice would show.
   return MakeBanner();
}

bool ShowStatusBanner(const BannerText& banner, double lifetimeSeconds)
{
   if (!MakeBanner())
      return false;
   if (!(banner == bannerText))
      bannerDrawn = false;
   bannerText = banner;
   bannerUp = true;
   bannerUntil = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(lifetimeSeconds * 1000));
   // A banner that could not be drawn hands the alert to VPX's notice.
   if (!FollowBanner())
   {
      bannerUp = false;
      return false;
   }
   return true;
}

void HideStatusBanner()
{
   bannerUp = false;
   HideBannerWindow();
}

void TickStatusBanner(bool release)
{
   if (release)
   {
      bannerUp = false;
      if (bannerWindow != nullptr)
         DestroyWindow(bannerWindow);
      bannerWindow = nullptr;
      bannerPlayfield = nullptr;
      if (bannerClass != 0)
         UnregisterClassW(BANNER_CLASS, ThisModule());
      bannerClass = 0;
      bannerDrawn = false;
      bannerShownNow = false;
      return;
   }
   if (bannerUp && Clock::now() >= bannerUntil)
      bannerUp = false;
   FollowBanner();
}

const char* StatusBannerBlocked(const std::string& prefPath)
{
   // Only the DirectX 9 build has an exclusive mode; the BGFX and OpenGL builds name themselves.
   // Wide throughout: a path outside the code page must not throw here.
   wchar_t exe[MAX_PATH] = { };
   GetModuleFileNameW(nullptr, exe, MAX_PATH);
   std::wstring name = std::filesystem::path(exe).filename().wstring();
   for (wchar_t& c : name)
      c = static_cast<wchar_t>(towlower(c));
   if (name.find(L"_bgfx") != std::wstring::npos || name.find(L"_gl") != std::wstring::npos)
      return nullptr;
   std::ifstream ini(std::filesystem::path(prefPath) / "VPinballX.ini");
   bool inPlayer = false;
   for (std::string line; std::getline(ini, line);)
   {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos)
         continue;
      line = line.substr(start);
      if (line[0] == '[')
      {
         inPlayer = Lower(line).rfind("[player]", 0) == 0;
         continue;
      }
      if (!inPlayer || Lower(line).rfind("playfieldfullscreen", 0) != 0)
         continue;
      const size_t eq = line.find('=');
      if (eq != std::string::npos && line.find_first_not_of(" \t\r", eq + 1) != std::string::npos
         && line[line.find_first_not_of(" \t\r", eq + 1)] == '2')
         return "VPX's DirectX 9 build in exclusive fullscreen (PlayfieldFullScreen = 2)";
   }
   return nullptr;
}

}
