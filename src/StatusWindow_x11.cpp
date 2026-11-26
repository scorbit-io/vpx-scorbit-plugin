// license:GPLv3+

#include "StatusWindow.h"
#include "StatusLightRaster.h"

#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// An override-redirect window with an empty input shape: the window manager never manages it,
// so it is never given focus, and it takes no input. VPX pauses whenever its playfield loses focus.
//
// xcb rather than Xlib: xcb reports an error with the request that caused it, where Xlib calls a
// process-wide handler that SDL also swaps and whose default exits. Loaded with dlopen, so the
// plugin still loads where there is no X at all. The connection is the plugin's own, created and
// closed on the API thread and used in between only by the light's thread, which draws on a
// timer of its own: the API thread's tick (250 ms) is slower than a dot's dark phase (100 ms).
namespace Scorbit
{

namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int TICK_MS = 25;           // under the shortest dark phase of a dot (100 ms)
constexpr double INSET_PT = 16;
constexpr int RING_STEPS = 16;        // as on Windows: about 10 redraws a second while the ring pulses
constexpr uint32_t SHAPE_ALPHA = 64;  // without a compositor, pixels at least this opaque are kept
constexpr auto FRONT_EVERY = 250ms;   // the active window, the playfield's place and its monitor
constexpr auto COMPOSITOR_EVERY = 1s;
constexpr auto RAISE_GAP = 50ms;      // so two windows that each keep themselves on top cannot spin
// The caption beside the light while the cursor hovers over it (docs/status-notices.md, "Hover"),
// in a core X font the server draws itself: the plugin loads no font library.
constexpr auto HOVER_EVERY = 50ms;
constexpr double CAPTION_GAP_PT = 8;
constexpr double CAPTION_MAX_WIDTH_PT = 360;
constexpr double CAPTION_PAD_X_PT = 8;
constexpr double CAPTION_PAD_Y_PT = 5;
constexpr uint32_t CAPTION_BACKGROUND = 0x99000000; // black at 0.6, as behind the light
constexpr uint32_t CAPTION_TEXT = 0xffffffff;

#define SCORBIT_XCB_CORE(X) \
   X(xcb_connect) X(xcb_disconnect) X(xcb_connection_has_error) X(xcb_get_setup) X(xcb_get_file_descriptor) \
   X(xcb_get_maximum_request_length) X(xcb_get_extension_data) X(xcb_generate_id) X(xcb_flush) X(xcb_poll_for_event) \
   X(xcb_setup_roots_iterator) X(xcb_screen_next) X(xcb_screen_allowed_depths_iterator) X(xcb_depth_next) \
   X(xcb_depth_visuals_iterator) X(xcb_visualtype_next) X(xcb_setup_pixmap_formats) X(xcb_setup_pixmap_formats_length) \
   X(xcb_intern_atom) X(xcb_intern_atom_reply) X(xcb_get_property) X(xcb_get_property_reply) \
   X(xcb_get_property_value) X(xcb_get_property_value_length) X(xcb_get_input_focus) X(xcb_get_input_focus_reply) \
   X(xcb_query_tree) X(xcb_query_tree_reply) X(xcb_query_tree_children) X(xcb_query_tree_children_length) \
   X(xcb_get_window_attributes) X(xcb_get_window_attributes_reply) X(xcb_get_geometry) X(xcb_get_geometry_reply) \
   X(xcb_translate_coordinates) X(xcb_translate_coordinates_reply) X(xcb_get_selection_owner) \
   X(xcb_get_selection_owner_reply) X(xcb_create_colormap) X(xcb_create_window) X(xcb_change_window_attributes) \
   X(xcb_change_property) X(xcb_configure_window) X(xcb_map_window) X(xcb_unmap_window) X(xcb_create_gc) \
   X(xcb_put_image) X(xcb_query_pointer) X(xcb_query_pointer_reply) X(xcb_open_font) \
   X(xcb_query_text_extents) X(xcb_query_text_extents_reply) X(xcb_image_text_8) X(xcb_change_gc) \
   X(xcb_poly_fill_rectangle) X(xcb_fill_poly) X(xcb_poly_fill_arc) X(xcb_poly_text_8) X(xcb_list_fonts) \
   X(xcb_list_fonts_reply) X(xcb_list_fonts_names_iterator) X(xcb_str_next) X(xcb_str_name) X(xcb_str_name_length)
#define SCORBIT_XCB_SHAPE(X) \
   X(xcb_shape_id) X(xcb_shape_query_version) X(xcb_shape_query_version_reply) X(xcb_shape_rectangles)
#define SCORBIT_XCB_RANDR(X) \
   X(xcb_randr_id) X(xcb_randr_get_monitors) X(xcb_randr_get_monitors_reply) \
   X(xcb_randr_get_monitors_monitors_iterator) X(xcb_randr_monitor_info_next)

struct Xcb
{
   void* core = nullptr;
   void* shape = nullptr;
   void* randr = nullptr;   // optional: without it the light uses the whole screen
#define SCORBIT_XCB_MEMBER(f) decltype(&::f) f = nullptr;
   SCORBIT_XCB_CORE(SCORBIT_XCB_MEMBER)
   SCORBIT_XCB_SHAPE(SCORBIT_XCB_MEMBER)
   SCORBIT_XCB_RANDR(SCORBIT_XCB_MEMBER)
#undef SCORBIT_XCB_MEMBER

   // Every dlsym, or false naming the library that is missing or short.
   bool Load(std::string& why)
   {
      core = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
      shape = dlopen("libxcb-shape.so.0", RTLD_NOW | RTLD_LOCAL);
      if (core == nullptr || shape == nullptr)
      {
         why = core == nullptr ? "libxcb.so.1 is not installed" : "libxcb-shape.so.0 is not installed";
         return false;
      }
      bool ok = true;
#define SCORBIT_XCB_LOAD(lib, f) ok = ok && (f = reinterpret_cast<decltype(f)>(dlsym(lib, #f))) != nullptr;
#define SCORBIT_XCB_LOAD_CORE(f) SCORBIT_XCB_LOAD(core, f)
#define SCORBIT_XCB_LOAD_SHAPE(f) SCORBIT_XCB_LOAD(shape, f)
#define SCORBIT_XCB_LOAD_RANDR(f) SCORBIT_XCB_LOAD(randr, f)
      SCORBIT_XCB_CORE(SCORBIT_XCB_LOAD_CORE)
      SCORBIT_XCB_SHAPE(SCORBIT_XCB_LOAD_SHAPE)
      if (!ok)
      {
         why = "libxcb lacks a function the light needs";
         return false;
      }
      randr = dlopen("libxcb-randr.so.0", RTLD_NOW | RTLD_LOCAL);
      if (randr != nullptr)
      {
         SCORBIT_XCB_RANDR(SCORBIT_XCB_LOAD_RANDR)
      }
      if (!ok)
         xcb_randr_id = nullptr;
#undef SCORBIT_XCB_LOAD
#undef SCORBIT_XCB_LOAD_CORE
#undef SCORBIT_XCB_LOAD_SHAPE
#undef SCORBIT_XCB_LOAD_RANDR
      return true;
   }

   void Unload()
   {
      for (void* lib : { randr, shape, core })
         if (lib != nullptr)
            dlclose(lib);
      *this = Xcb();
   }
};

struct Rect
{
   int x = 0, y = 0, w = 0, h = 0;
   bool operator==(const Rect&) const = default;
};

// What the API thread asks for; the light's thread copies it on each tick.
struct Wanted
{
   int corner = 0;
   LightView view;
   Clock::time_point since;
   uint8_t alpha = 255;
   bool flash = false;     // a claim flash, stamped when asked for, as on Windows
   Clock::time_point flashAt;
   std::string caption;    // shown beside the light while the cursor hovers over it
};

std::mutex wantedLock;
Wanted wanted;
std::atomic<bool> stopping { false };
std::atomic<bool> lost { false };   // the light's connection failed; the API thread hides it
std::thread worker;
std::string unavailable;   // API thread: why the light last failed to show, empty if it did not

// A connection of the plugin's own to the X server, and what the light and the banner both ask of
// it. Each has its own: the light's is used by its thread, the banner's by the API thread.
class Display
{
public:
   // API thread. False with why set if there is no X server to draw on or it lacks what is needed.
   bool Connect(std::string& why)
   {
      if (!x.Load(why))
         return false;
      int screenNumber = 0;
      c = x.xcb_connect(nullptr, &screenNumber);
      if (c == nullptr || x.xcb_connection_has_error(c))
      {
         why = "cannot connect to the X display";
         return false;
      }
      const xcb_setup_t* setup = x.xcb_get_setup(c);
      for (auto it = x.xcb_setup_roots_iterator(setup); it.rem > 0; x.xcb_screen_next(&it), --screenNumber)
         if (screenNumber == 0)
         {
            screen = it.data;
            break;
         }
      if (screen == nullptr)
      {
         why = "the X display has no such screen";
         return false;
      }
      root = screen->root;
      lsbImage = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;

      const xcb_query_extension_reply_t* shapeExt = x.xcb_get_extension_data(c, x.xcb_shape_id);
      xcb_shape_query_version_reply_t* version = shapeExt != nullptr && shapeExt->present
         ? x.xcb_shape_query_version_reply(c, x.xcb_shape_query_version(c), nullptr) : nullptr;
      const bool inputShape = version != nullptr && (version->major_version > 1 || version->minor_version >= 1);
      std::free(version);
      if (!inputShape)
      {
         why = "the X server has no input shapes (SHAPE 1.1)";
         return false;
      }
      if (x.xcb_randr_id != nullptr)
      {
         const xcb_query_extension_reply_t* randrExt = x.xcb_get_extension_data(c, x.xcb_randr_id);
         haveRandr = randrExt != nullptr && randrExt->present;
      }

      const std::string cm = "_NET_WM_CM_S" + std::to_string(ScreenIndex(setup));
      const char* const names[] = { "_NET_ACTIVE_WINDOW", "_NET_WM_PID", cm.c_str(), "_NET_WM_WINDOW_TYPE",
         "_NET_WM_WINDOW_TYPE_NOTIFICATION" };
      xcb_intern_atom_cookie_t cookies[std::size(names)];
      for (size_t i = 0; i < std::size(names); i++)
         cookies[i] = x.xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(names[i])), names[i]);
      xcb_atom_t* const atoms[] = { &activeAtom, &pidAtom, &compositorAtom, &typeAtom, &notificationAtom };
      for (size_t i = 0; i < std::size(names); i++)
         if (xcb_intern_atom_reply_t* reply = x.xcb_intern_atom_reply(c, cookies[i], nullptr))
         {
            *atoms[i] = reply->atom;
            std::free(reply);
         }

      return true;
   }

   // Once nothing else uses the connection: for the light, once its thread has stopped or never ran.
   // Closing it frees everything the server holds for it: windows, colormaps, GCs, fonts, the root
   // event mask.
   void Close()
   {
      if (c != nullptr)
         x.xcb_disconnect(c);
      c = nullptr;
      x.Unload();
   }

protected:
   Xcb x;
   xcb_connection_t* c = nullptr;
   xcb_screen_t* screen = nullptr;
   xcb_window_t root = XCB_WINDOW_NONE;
   xcb_atom_t activeAtom = XCB_ATOM_NONE, pidAtom = XCB_ATOM_NONE, compositorAtom = XCB_ATOM_NONE;
   xcb_atom_t typeAtom = XCB_ATOM_NONE, notificationAtom = XCB_ATOM_NONE;
   bool haveRandr = false;
   bool lsbImage = true;

   int ScreenIndex(const xcb_setup_t* setup)
   {
      int i = 0;
      for (auto it = x.xcb_setup_roots_iterator(setup); it.rem > 0 && it.data != screen; x.xcb_screen_next(&it))
         i++;
      return i;
   }

   // The width of 8-bit text in a core font, with the font's ascent and descent. False when the
   // font is not open.
   bool TextExtents(xcb_font_t f, const std::string& text, int& ascent, int& descent, int& width)
   {
      std::vector<xcb_char2b_t> chars(text.size());
      for (size_t i = 0; i < text.size(); i++)
         chars[i] = { 0, static_cast<uint8_t>(text[i]) };
      xcb_query_text_extents_reply_t* reply = x.xcb_query_text_extents_reply(c,
         x.xcb_query_text_extents(c, f, static_cast<uint32_t>(chars.size()), chars.data()), nullptr);
      if (reply == nullptr)
         return false;
      ascent = reply->font_ascent;
      descent = reply->font_descent;
      width = reply->overall_width;
      std::free(reply);
      return true;
   }

   // Text as a core font takes it: Latin-1 for what UTF-8 carries of it, '?' for the rest.
   static std::string Latin1(const std::string& utf8)
   {
      std::string out;
      for (size_t i = 0; i < utf8.size();)
      {
         const unsigned char b = static_cast<unsigned char>(utf8[i]);
         if (b < 0x80)
         {
            out.push_back(static_cast<char>(b));
            i++;
         }
         else if ((b & 0xe0) == 0xc0 && i + 1 < utf8.size())
         {
            const unsigned int code = ((b & 0x1fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3fu);
            out.push_back(code < 0x100 ? static_cast<char>(code) : '?');
            i += 2;
         }
         else
         {
            out.push_back('?');
            i += (b & 0xf0) == 0xf0 ? 4 : (b & 0xe0) == 0xe0 ? 3 : 1;
         }
      }
      return out;
   }

   // Within the width, and within the 255 bytes one xcb_image_text_8 draws. Unmeasurable counts as fitting.
   bool Fits(xcb_font_t f, const std::string& line, int maxWidth)
   {
      int ascent = 0, descent = 0, width = 0;
      return line.size() <= 255 && (!TextExtents(f, line, ascent, descent, width) || width <= maxWidth);
   }

   // Greedy word wrap within the width, measured by the server.
   std::vector<std::string> Wrap(xcb_font_t f, const std::string& text, int maxWidth)
   {
      std::vector<std::string> lines;
      std::string line;
      int ascent = 0, descent = 0, width = 0;
      for (size_t at = 0; at <= text.size();)
      {
         size_t end = text.find_first_of(" \n", at);
         if (end == std::string::npos)
            end = text.size();
         const std::string word = text.substr(at, end - at);
         const std::string candidate = line.empty() ? word : line + " " + word;
         if (!line.empty() && TextExtents(f, candidate, ascent, descent, width) && width > maxWidth)
         {
            lines.push_back(line);
            line = word;
         }
         else
            line = candidate;
         // A word too wide on its own is broken at the edge: the longest prefix that fits.
         while (line.size() > 1 && !Fits(f, line, maxWidth))
         {
            size_t lo = 1, hi = line.size() - 1;
            while (lo < hi)
            {
               const size_t mid = (lo + hi + 1) / 2;
               if (Fits(f, line.substr(0, mid), maxWidth))
                  lo = mid;
               else
                  hi = mid - 1;
            }
            lines.push_back(line.substr(0, lo));
            line.erase(0, lo);
         }
         if (end < text.size() && text[end] == '\n')
         {
            lines.push_back(line);
            line.clear();
         }
         at = end + 1;
      }
      if (!line.empty() || lines.empty())
         lines.push_back(line);
      return lines;
   }

   // Xft.dpi, which desktops set for a scaled display, over the 96 dpi of an unscaled one.
   double ReadScale()
   {
      double dpi = 96;
      if (xcb_get_property_reply_t* reply = x.xcb_get_property_reply(c,
             x.xcb_get_property(c, 0, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 16384), nullptr))
      {
         const std::string_view db(static_cast<const char*>(x.xcb_get_property_value(reply)),
            static_cast<size_t>(std::max(0, x.xcb_get_property_value_length(reply))));
         for (size_t at = 0; at < db.size();)
         {
            size_t end = db.find('\n', at);
            if (end == std::string_view::npos)
               end = db.size();
            const std::string_view line = db.substr(at, end - at);
            if (line.starts_with("Xft.dpi:"))
            {
               const std::string value(line.substr(8));
               if (const double v = std::atof(value.c_str()); v > 0)
                  dpi = v;
            }
            at = end + 1;
         }
         std::free(reply);
      }
      return std::clamp(dpi / 96.0, 1.0, 4.0);
   }

   xcb_window_t ReadWindow(xcb_window_t w, xcb_atom_t property, bool& present)
   {
      xcb_window_t value = XCB_WINDOW_NONE;
      present = false;
      if (xcb_get_property_reply_t* reply = x.xcb_get_property_reply(c,
             x.xcb_get_property(c, 0, w, property, XCB_ATOM_WINDOW, 0, 1), nullptr))
      {
         present = reply->type == XCB_ATOM_WINDOW;
         if (present && x.xcb_get_property_value_length(reply) >= 4)
            std::memcpy(&value, x.xcb_get_property_value(reply), 4);
         std::free(reply);
      }
      return value;
   }

   // VPX plays only while its playfield holds keyboard focus, so while VPX is in front, the
   // active window is the playfield. Found by its _NET_WM_PID, which SDL sets, on it or on the
   // nearest ancestor that has one; a window that names another process is not VPX's.
   xcb_window_t FindPlayfield()
   {
      bool ewmh = false;
      xcb_window_t w = activeAtom != XCB_ATOM_NONE ? ReadWindow(root, activeAtom, ewmh) : xcb_window_t(XCB_WINDOW_NONE);
      if (!ewmh)
         if (xcb_get_input_focus_reply_t* focus = x.xcb_get_input_focus_reply(c, x.xcb_get_input_focus(c), nullptr))
         {
            w = focus->focus;
            std::free(focus);
         }
      const uint32_t self = static_cast<uint32_t>(getpid());
      for (int hops = 0; w != XCB_WINDOW_NONE && w != XCB_INPUT_FOCUS_POINTER_ROOT && w != root && hops < 8; hops++)
      {
         if (xcb_get_property_reply_t* reply = x.xcb_get_property_reply(c,
                x.xcb_get_property(c, 0, w, pidAtom, XCB_ATOM_CARDINAL, 0, 1), nullptr))
         {
            uint32_t pid = 0;
            const bool has = reply->type == XCB_ATOM_CARDINAL && x.xcb_get_property_value_length(reply) >= 4;
            if (has)
               std::memcpy(&pid, x.xcb_get_property_value(reply), 4);
            std::free(reply);
            if (has)
               return pid == self ? w : xcb_window_t(XCB_WINDOW_NONE);
         }
         xcb_query_tree_reply_t* tree = x.xcb_query_tree_reply(c, x.xcb_query_tree(c, w), nullptr);
         if (tree == nullptr)
            return XCB_WINDOW_NONE;
         w = tree->parent;
         std::free(tree);
      }
      return XCB_WINDOW_NONE;
   }

   // A window's place and size on the root, or false if it went.
   bool WindowRect(xcb_window_t w, Rect& out)
   {
      xcb_get_geometry_reply_t* geometry = x.xcb_get_geometry_reply(c, x.xcb_get_geometry(c, w), nullptr);
      xcb_translate_coordinates_reply_t* origin
         = x.xcb_translate_coordinates_reply(c, x.xcb_translate_coordinates(c, w, root, 0, 0), nullptr);
      const bool ok = geometry != nullptr && origin != nullptr;
      if (ok)
         out = { origin->dst_x, origin->dst_y, geometry->width, geometry->height };
      std::free(geometry);
      std::free(origin);
      return ok;
   }

   // The topmost viewable window the window manager manages, or none. A window of ours goes directly
   // above it, so override-redirect windows over it (menus, a screen locker) stay above ours.
   xcb_window_t TopManaged()
   {
      xcb_query_tree_reply_t* tree = x.xcb_query_tree_reply(c, x.xcb_query_tree(c, root), nullptr);
      if (tree == nullptr)
         return XCB_WINDOW_NONE;
      const xcb_window_t* children = x.xcb_query_tree_children(tree);   // bottom to top
      const int count = x.xcb_query_tree_children_length(tree);
      std::vector<xcb_get_window_attributes_cookie_t> cookies(static_cast<size_t>(count));
      for (int i = count - 1; i >= 0; i--)
         cookies[static_cast<size_t>(i)] = x.xcb_get_window_attributes(c, children[i]);
      xcb_window_t top = XCB_WINDOW_NONE;
      for (int i = count - 1; i >= 0; i--)
      {
         // Every reply is collected, so none is left queued on the connection.
         xcb_get_window_attributes_reply_t* attributes = x.xcb_get_window_attributes_reply(c, cookies[static_cast<size_t>(i)], nullptr);
         if (top == XCB_WINDOW_NONE && attributes != nullptr && !attributes->override_redirect
            && attributes->map_state == XCB_MAP_STATE_VIEWABLE)
            top = children[i];
         std::free(attributes);
      }
      std::free(tree);
      return top;
   }

   bool Composited()
   {
      if (compositorAtom == XCB_ATOM_NONE)
         return false;
      xcb_get_selection_owner_reply_t* reply
         = x.xcb_get_selection_owner_reply(c, x.xcb_get_selection_owner(c, compositorAtom), nullptr);
      const bool owned = reply != nullptr && reply->owner != XCB_WINDOW_NONE;
      std::free(reply);
      return owned;
   }

};

class Light : public Display
{
public:
   // API thread. False with why set if there is no X server to draw on or it lacks what is needed.
   bool Open(std::string& why)
   {
      if (!Connect(why))
         return false;
      if (!CreateWindow(x.xcb_get_setup(c), why))
         return false;
      CreateCaption();
      scale = ReadScale();
      x.xcb_flush(c);
      if (x.xcb_connection_has_error(c))
      {
         why = "the X connection failed while creating the light";
         return false;
      }
      return true;
   }

   // The light's thread.
   void Run()
   {
      pollfd fd { x.xcb_get_file_descriptor(c), POLLIN, 0 };
      while (!stopping.load(std::memory_order_acquire) && !x.xcb_connection_has_error(c))
      {
         poll(&fd, 1, TICK_MS);
         Events();
         Tick();
         x.xcb_flush(c);
      }
      if (x.xcb_connection_has_error(c))
         lost.store(true, std::memory_order_release);
   }

private:
   bool CreateWindow(const xcb_setup_t* setup, std::string& why)
   {
      // An ARGB visual lets a compositor blend the light; without one, the light is clipped to its disc.
      xcb_visualid_t visual = screen->root_visual;
      uint8_t depth = screen->root_depth;
      for (auto d = x.xcb_screen_allowed_depths_iterator(screen); d.rem > 0 && !argb; x.xcb_depth_next(&d))
         if (d.data->depth == 32)
            for (auto v = x.xcb_depth_visuals_iterator(d.data); v.rem > 0; x.xcb_visualtype_next(&v))
               if (v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && v.data->red_mask == 0xff0000 && v.data->green_mask == 0xff00
                  && v.data->blue_mask == 0xff)
               {
                  visual = v.data->visual_id;
                  depth = 32;
                  argb = true;
                  lightVisual = visual;
                  break;
               }
      // The frame is 0xAARRGGBB words, which the server takes as they are only at 32 bits a pixel.
      bool wordPixels = false;
      const xcb_format_t* formats = x.xcb_setup_pixmap_formats(setup);
      for (int i = 0; i < x.xcb_setup_pixmap_formats_length(setup); i++)
         if (formats[i].depth == depth && formats[i].bits_per_pixel == 32)
            wordPixels = true;
      if (!wordPixels || (!argb && depth != 24))
      {
         why = "the X screen is not 24-bit or 32-bit TrueColor";
         return false;
      }

      window = x.xcb_generate_id(c);
      uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
      std::vector<uint32_t> values { 0, 0, 1, XCB_EVENT_MASK_EXPOSURE };
      if (argb)
      {
         // A visual other than its parent's needs a colormap of its own.
         const xcb_colormap_t colormap = x.xcb_generate_id(c);
         x.xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, root, visual);
         mask |= XCB_CW_COLORMAP;
         values.push_back(colormap);
      }
      x.xcb_create_window(c, depth, window, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, values.data());
      x.xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, window, 0, 0, 0, nullptr);
      // Named, so a user can tell their compositor to leave it alone (no shadow, no fade).
      static constexpr char CLASS[] = "scorbit-status-light\0Scorbit";
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(CLASS), CLASS);
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 20, "Scorbit status light");
      if (typeAtom != XCB_ATOM_NONE && notificationAtom != XCB_ATOM_NONE)
         x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, typeAtom, XCB_ATOM_ATOM, 32, 1, &notificationAtom);
      gc = x.xcb_generate_id(c);
      x.xcb_create_gc(c, gc, window, 0, nullptr);
      // Ours alone on the server: the window manager's selection on root is unaffected.
      const uint32_t rootEvents = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
      x.xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &rootEvents);
      return true;
   }

   // A second window of the light's kind, with a core font the server draws: a scalable Helvetica
   // where the server has one, else its "fixed". No font means no caption, and the light is unaffected.
   void CreateCaption()
   {
      caption = x.xcb_generate_id(c);
      uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
      std::vector<uint32_t> values { Pixel(CAPTION_BACKGROUND), 0, 1, XCB_EVENT_MASK_EXPOSURE };
      xcb_visualid_t visual = screen->root_visual;
      uint8_t depth = screen->root_depth;
      if (argb)
      {
         // The light's visual and a colormap of the caption's own.
         visual = lightVisual;
         depth = 32;
         const xcb_colormap_t colormap = x.xcb_generate_id(c);
         x.xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, colormap, root, visual);
         mask |= XCB_CW_COLORMAP;
         values.push_back(colormap);
      }
      x.xcb_create_window(c, depth, caption, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, values.data());
      x.xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, caption, 0, 0, 0, nullptr);
      static constexpr char CLASS[] = "scorbit-status-caption\0Scorbit";
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, caption, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(CLASS), CLASS);
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, caption, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 22, "Scorbit status caption");
      if (typeAtom != XCB_ATOM_NONE && notificationAtom != XCB_ATOM_NONE)
         x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, caption, typeAtom, XCB_ATOM_ATOM, 32, 1, &notificationAtom);
      font = x.xcb_generate_id(c);
      const int px = static_cast<int>(std::lround(12 * ReadScale()));
      const std::string name = "-*-helvetica-medium-r-normal--" + std::to_string(px) + "-*-*-*-*-*-iso8859-1";
      // Checked: an unknown font name is an error on the open, which the reply of a query shows.
      x.xcb_open_font(c, font, static_cast<uint16_t>(name.size()), name.c_str());
      int width = 0;
      if (!TextExtents(font, "x", fontAscent, fontDescent, width))
      {
         x.xcb_open_font(c, font, 5, "fixed");
         if (!TextExtents(font, "x", fontAscent, fontDescent, width))
         {
            font = 0;
            return;
         }
      }
      const uint32_t gcValues[] = { Pixel(CAPTION_TEXT), Pixel(CAPTION_BACKGROUND), font };
      captionGc = x.xcb_generate_id(c);
      x.xcb_create_gc(c, captionGc, caption, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT, gcValues);
   }

   // A premultiplied ARGB colour as the caption's visual takes it, at the light's opacity as Put
   // applies it: every channel scaled, then without alpha at depth 24.
   uint32_t Pixel(uint32_t argbColour, uint8_t alpha = 255) const
   {
      uint32_t q = 0;
      for (int shift = 0; shift < 32; shift += 8)
         q |= (((argbColour >> shift & 0xff) * alpha + 127) / 255) << shift;
      return argb ? q : q & 0xffffff;
   }

   // Beside the light on the side toward the monitor's centre, level with it, at the light's
   // opacity. Drawn again only when the text or opacity changes; filled and written by the server.
   void ShowCaption(const std::string& text, const Rect& lightAt, uint8_t alpha)
   {
      if (font == 0)
         return;
      const int padX = static_cast<int>(std::lround(CAPTION_PAD_X_PT * scale));
      const int padY = static_cast<int>(std::lround(CAPTION_PAD_Y_PT * scale));
      if (text != captionText)
      {
         const std::string latin = Latin1(text);
         captionLines = Wrap(font, latin, static_cast<int>(std::lround(CAPTION_MAX_WIDTH_PT * scale)));
         int ascent = 0, descent = 0, width = 0, widest = 1;
         for (const std::string& line : captionLines)
            if (TextExtents(font, line, ascent, descent, width))
               widest = std::max(widest, width);
         lineHeight = std::max(1, fontAscent + fontDescent);
         captionSize = { 0, 0, widest + 2 * padX, static_cast<int>(captionLines.size()) * lineHeight + 2 * padY };
         captionText = text;
         captionDirty = true;
      }
      if (alpha != captionAlpha)
      {
         // Also the window's own background, which the server paints on an expose.
         const uint32_t background[] = { Pixel(CAPTION_BACKGROUND, alpha) };
         x.xcb_change_window_attributes(c, caption, XCB_CW_BACK_PIXEL, background);
         captionAlpha = alpha;
         captionDirty = true;
      }
      const int gap = static_cast<int>(std::lround(CAPTION_GAP_PT * scale));
      const bool lightOnRight = lightAt.x + lightAt.w / 2 > monitor.x + monitor.w / 2;
      const int xAt = lightOnRight ? lightAt.x - gap - captionSize.w : lightAt.x + lightAt.w + gap;
      const int lowest = std::max(monitor.y, monitor.y + monitor.h - captionSize.h);
      const int yAt = std::clamp(lightAt.y + lightAt.h / 2 - captionSize.h / 2, monitor.y, lowest);
      const Rect at { xAt, yAt, captionSize.w, captionSize.h };
      if (!(at == captionPlaced))
      {
         const uint32_t geometry[] = { static_cast<uint32_t>(at.x), static_cast<uint32_t>(at.y), static_cast<uint32_t>(at.w),
            static_cast<uint32_t>(at.h) };
         x.xcb_configure_window(c, caption, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            geometry);
         captionPlaced = at;
         captionDirty = true;
      }
      if (!captionMapped)
      {
         // Directly above the light, which is itself stacked above the playfield.
         const uint32_t values[] = { window, XCB_STACK_MODE_ABOVE };
         x.xcb_configure_window(c, caption, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
         x.xcb_map_window(c, caption);
         captionMapped = true;
         captionDirty = true;
      }
      if (captionDirty)
      {
         const uint32_t fill[] = { Pixel(CAPTION_BACKGROUND, alpha) };
         x.xcb_change_gc(c, captionGc, XCB_GC_FOREGROUND, fill);
         const xcb_rectangle_t whole { 0, 0, static_cast<uint16_t>(at.w), static_cast<uint16_t>(at.h) };
         x.xcb_poly_fill_rectangle(c, caption, captionGc, 1, &whole);
         // Image text fills each glyph's box with the background, so both follow the opacity.
         const uint32_t ink[] = { Pixel(CAPTION_TEXT, alpha), Pixel(CAPTION_BACKGROUND, alpha) };
         x.xcb_change_gc(c, captionGc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, ink);
         int baseline = padY + fontAscent;
         for (const std::string& line : captionLines)
         {
            x.xcb_image_text_8(c, static_cast<uint8_t>(std::min<size_t>(line.size(), 255)), caption, captionGc, static_cast<int16_t>(padX),
               static_cast<int16_t>(baseline), line.c_str());
            baseline += lineHeight;
         }
         captionDirty = false;
      }
   }

   void HideCaption()
   {
      if (captionMapped)
         x.xcb_unmap_window(c, caption);
      captionMapped = false;
   }

   // The pointer's place, read with no input of our own, so both windows stay click-through.
   void Hover(const std::string& text, const Rect& lightAt, uint8_t alpha, Clock::time_point now)
   {
      if (now < nextHover)
         return;
      nextHover = now + HOVER_EVERY;
      xcb_query_pointer_reply_t* pointer = x.xcb_query_pointer_reply(c, x.xcb_query_pointer(c, root), nullptr);
      if (pointer == nullptr)
         return;
      const int px = pointer->root_x, py = pointer->root_y;
      std::free(pointer);
      const bool over = px >= lightAt.x && px < lightAt.x + lightAt.w && py >= lightAt.y && py < lightAt.y + lightAt.h;
      // X has no cheap word on whether the cursor is hidden; the movement rule alone applies.
      if (hover.Tick(px, py, over, false, now) && !text.empty())
         ShowCaption(text, lightAt, alpha);
      else
         HideCaption();
   }

   // The monitor under the playfield's centre, or the whole screen without RandR 1.5.
   bool PlayfieldMonitor(xcb_window_t playfield, Rect& out)
   {
      Rect area;
      if (!WindowRect(playfield, area))
         return false;
      const int cx = area.x + area.w / 2, cy = area.y + area.h / 2;
      out = { 0, 0, screen->width_in_pixels, screen->height_in_pixels };
      if (xcb_get_geometry_reply_t* rootSize = x.xcb_get_geometry_reply(c, x.xcb_get_geometry(c, root), nullptr))
      {
         out = { 0, 0, rootSize->width, rootSize->height }; // the screen as it is now, after any mode change
         std::free(rootSize);
      }
      if (!haveRandr)
         return true;
      if (xcb_randr_get_monitors_reply_t* monitors
         = x.xcb_randr_get_monitors_reply(c, x.xcb_randr_get_monitors(c, root, 1), nullptr))
      {
         bool first = true;
         for (auto it = x.xcb_randr_get_monitors_monitors_iterator(monitors); it.rem > 0; x.xcb_randr_monitor_info_next(&it))
         {
            const Rect m { it.data->x, it.data->y, it.data->width, it.data->height };
            if (first || (cx >= m.x && cx < m.x + m.w && cy >= m.y && cy < m.y + m.h))
               out = m;
            if (cx >= m.x && cx < m.x + m.w && cy >= m.y && cy < m.y + m.h)
               break;
            first = false;
         }
         std::free(monitors);
      }
      return true;
   }

   void Events()
   {
      while (xcb_generic_event_t* event = x.xcb_poll_for_event(c))
      {
         switch (event->response_type & 0x7f)
         {
         case 0: // an error, e.g. a window that went between two requests: the next tick asks again
            break;
         case XCB_EXPOSE:
            if (reinterpret_cast<xcb_expose_event_t*>(event)->window == caption)
               captionDirty = true;
            else
               redraw = true;
            break;
         case XCB_PROPERTY_NOTIFY:
            if (reinterpret_cast<xcb_property_notify_event_t*>(event)->atom == activeAtom)
               frontStale = true;
            break;
         // Another top-level window moved or was restacked, perhaps over the light. Not for another
         // override-redirect window (a menu, a tooltip, a screen locker), which must stay above.
         case XCB_CONFIGURE_NOTIFY:
         {
            const auto* configure = reinterpret_cast<xcb_configure_notify_event_t*>(event);
            if (configure->window != window && !configure->override_redirect)
               raise = true;
            frontStale = true;
            break;
         }
         case XCB_MAP_NOTIFY:
         {
            const auto* map = reinterpret_cast<xcb_map_notify_event_t*>(event);
            if (map->window != window && !map->override_redirect)
               raise = true;
            break;
         }
         default:
            break;
         }
         std::free(event);
      }
   }

   // Directly above the topmost managed window (TopManaged). With none, it is left where it is.
   void StackAbove()
   {
      lastRaise = Clock::now();
      raise = false;
      const xcb_window_t sibling = TopManaged();
      if (sibling == XCB_WINDOW_NONE)
         return;
      const uint32_t values[] = { sibling, XCB_STACK_MODE_ABOVE };
      x.xcb_configure_window(c, window, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
      if (captionMapped)
      {
         // The caption stays directly above the light, wherever the light went.
         const uint32_t above[] = { window, XCB_STACK_MODE_ABOVE };
         x.xcb_configure_window(c, caption, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, above);
      }
   }

   void Tick()
   {
      Wanted w;
      {
         std::lock_guard guard(wantedLock);
         w = wanted;
      }
      const auto now = Clock::now();
      if (frontStale || now >= nextFront)
      {
         const xcb_window_t playfield = FindPlayfield();
         inFront = playfield != XCB_WINDOW_NONE && PlayfieldMonitor(playfield, monitor);
         frontStale = false;
         nextFront = now + FRONT_EVERY;
      }
      if (!inFront)
      {
         // Shown only while VPX is the application in front, so it never sits over another one.
         if (mapped)
            x.xcb_unmap_window(c, window);
         mapped = false;
         HideCaption();
         return;
      }
      if (now >= nextCompositor)
      {
         if (const bool composited = Composited(); composited != compositing)
         {
            compositing = composited;
            shapeStale = true;
         }
         nextCompositor = now + COMPOSITOR_EVERY;
      }

      double flashSeconds = w.flash ? std::chrono::duration<double>(now - w.flashAt).count() : -1;
      if (flashSeconds >= LightGeometry::FLASH_S)
         flashSeconds = -1;
      LightPhase phase = PhaseAt(w.view, std::chrono::duration<double>(now - w.since).count(), flashSeconds);
      if (w.view.ring == LightRing::Rainbow)
         phase.ring = static_cast<uint8_t>(std::lround(std::round(phase.ring * (RING_STEPS - 1) / 255.0) * 255.0 / (RING_STEPS - 1)));

      const int size = std::max(1, static_cast<int>(std::lround(LightGeometry::DIAMETER * scale)));
      const int inset = static_cast<int>(std::lround(INSET_PT * scale));
      const Rect at {
         (w.corner == 1 || w.corner == 3) ? monitor.x + inset : monitor.x + monitor.w - inset - size,
         (w.corner == 3 || w.corner == 4) ? monitor.y + monitor.h - inset - size : monitor.y + inset,
         size, size,
      };
      if (!(at == placed))
      {
         const uint32_t geometry[] = { static_cast<uint32_t>(at.x), static_cast<uint32_t>(at.y), static_cast<uint32_t>(size),
            static_cast<uint32_t>(size) };
         x.xcb_configure_window(c, window,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geometry);
         if (at.w != placed.w)
            shapeStale = true;
         placed = at;
         redraw = true;
      }

      const bool changed = redraw || !(w.view == drawnView) || !(phase == drawnPhase) || w.alpha != drawnAlpha || size != frame.size;
      if (changed || shapeStale)
         Rasterize(w.view, phase, scale, frame);
      if (shapeStale)
         Shape();
      if (!mapped)
      {
         // Stacked first: a window maps at the top, over any override-redirect window.
         StackAbove();
         x.xcb_map_window(c, window);
         mapped = true;
      }
      else if (raise && now - lastRaise >= RAISE_GAP)
         StackAbove();
      if (changed)
      {
         Put(w.alpha);
         drawnView = w.view;
         drawnPhase = phase;
         drawnAlpha = w.alpha;
         redraw = false;
      }
      Hover(w.caption, placed, w.alpha, now);
   }

   // Always cut to the disc. Without a compositor the server shows each pixel's colour as it is,
   // so only the more opaque pixels are kept and the antialiased rim sits over black. A compositor
   // may stop compositing a fullscreen window even with the light above it (picom's
   // unredir-if-possible), and then the light would show as a black square.
   void Shape()
   {
      shapeStale = false;
      const uint32_t keep = argb && compositing ? 1 : SHAPE_ALPHA;
      std::vector<xcb_rectangle_t> runs;
      for (int y = 0; y < frame.size; y++)
         for (int x0 = 0; x0 < frame.size;)
         {
            if ((frame.pixels[static_cast<size_t>(y) * frame.size + x0] >> 24) < keep)
            {
               x0++;
               continue;
            }
            int x1 = x0;
            while (x1 < frame.size && (frame.pixels[static_cast<size_t>(y) * frame.size + x1] >> 24) >= keep)
               x1++;
            runs.push_back({ static_cast<int16_t>(x0), static_cast<int16_t>(y), static_cast<uint16_t>(x1 - x0), 1 });
            x0 = x1;
         }
      x.xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_YX_BANDED, window, 0, 0,
         static_cast<uint32_t>(runs.size()), runs.data());
   }

   // Opacity scales every premultiplied channel, which is a compositor's blend when there is one.
   void Put(uint8_t alpha)
   {
      const size_t count = frame.pixels.size();
      image.resize(count);
      const bool swap = lsbImage != (std::endian::native == std::endian::little);
      for (size_t i = 0; i < count; i++)
      {
         uint32_t p = frame.pixels[i];
         if (alpha != 255)
         {
            uint32_t q = 0;
            for (int shift = 0; shift < 32; shift += 8)
               q |= (((p >> shift & 0xff) * alpha + 127) / 255) << shift;
            p = q;
         }
         image[i] = swap ? ((p >> 24) | ((p >> 8) & 0xff00) | ((p << 8) & 0xff0000) | (p << 24)) : p;
      }
      // Split by rows to stay under the server's request size.
      const size_t maxBytes = static_cast<size_t>(x.xcb_get_maximum_request_length(c)) * 4 - 64;
      const int rowBytes = frame.size * 4;
      const int rows = std::max(1, static_cast<int>(maxBytes / static_cast<size_t>(rowBytes)));
      const uint8_t depth = argb ? 32 : screen->root_depth;
      for (int y = 0; y < frame.size; y += rows)
      {
         const int n = std::min(rows, frame.size - y);
         x.xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, window, gc, static_cast<uint16_t>(frame.size), static_cast<uint16_t>(n), 0,
            static_cast<int16_t>(y), 0, depth, static_cast<uint32_t>(n * rowBytes),
            reinterpret_cast<const uint8_t*>(image.data() + static_cast<size_t>(y) * frame.size));
      }
   }

   xcb_window_t window = XCB_WINDOW_NONE;
   xcb_gcontext_t gc = 0;
   bool argb = false;
   double scale = 1;
   xcb_visualid_t lightVisual = 0;

   // The caption.
   xcb_window_t caption = XCB_WINDOW_NONE;
   xcb_gcontext_t captionGc = 0;
   xcb_font_t font = 0;
   int fontAscent = 0, fontDescent = 0, lineHeight = 1;
   HoverTracker hover;
   Clock::time_point nextHover;
   std::string captionText;
   std::vector<std::string> captionLines;
   Rect captionSize, captionPlaced;
   bool captionMapped = false;
   bool captionDirty = true;
   uint8_t captionAlpha = 255;   // as the window and GC were created

   bool inFront = false;
   bool frontStale = true;
   bool mapped = false;
   bool raise = false;
   bool redraw = true;
   bool shapeStale = true;
   bool compositing = false;
   Clock::time_point nextFront, nextCompositor, lastRaise;
   Rect monitor, placed;
   LightView drawnView;
   LightPhase drawnPhase;
   uint8_t drawnAlpha = 0;
   LightFrame frame;
   std::vector<uint32_t> image;
};

// The status alert banner (docs/status-banner.md): a window of the light's kind, opaque, centred
// in the playfield a quarter of the way down. Static, so it needs no thread: the API thread's tick
// (TickStatusBanner) places, raises and hides it, on a connection of the banner's own.
constexpr uint32_t BANNER_YELLOW = 0xffd400;  // #FFD400
constexpr uint32_t BANNER_WHITE = 0xffffff;
constexpr uint32_t BANNER_BLACK = 0x000000;

class Banner : public Display
{
public:
   // API thread. False with why set when no banner can show: no X server, or no scalable font.
   bool Open(std::string& why)
   {
      if (!Connect(why))
         return false;
      // Opaque, so the root visual will do: no colormap, and no compositor needed.
      bool trueColor = false;
      for (auto d = x.xcb_screen_allowed_depths_iterator(screen); d.rem > 0; x.xcb_depth_next(&d))
         for (auto v = x.xcb_depth_visuals_iterator(d.data); v.rem > 0; x.xcb_visualtype_next(&v))
            if (v.data->visual_id == screen->root_visual)
               trueColor = v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && v.data->red_mask == 0xff0000
                  && v.data->green_mask == 0xff00 && v.data->blue_mask == 0xff;
      if (!trueColor)
      {
         why = "the X screen is not 24-bit TrueColor";
         return false;
      }
      scale = ReadScale();
      textPx = std::max(1, static_cast<int>(std::lround(BANNER_TEXT_HEIGHT * scale)));
      if (!OpenFonts(why))
         return false;

      window = x.xcb_generate_id(c);
      const uint32_t values[] = { Pixel(BANNER_BLACK), 0, 1, XCB_EVENT_MASK_EXPOSURE };
      x.xcb_create_window(c, screen->root_depth, window, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
         XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
      x.xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, window, 0, 0, 0, nullptr);
      static constexpr char CLASS[] = "scorbit-status-banner\0Scorbit";
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(CLASS), CLASS);
      x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 21, "Scorbit status banner");
      if (typeAtom != XCB_ATOM_NONE && notificationAtom != XCB_ATOM_NONE)
         x.xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, typeAtom, XCB_ATOM_ATOM, 32, 1, &notificationAtom);
      gc = x.xcb_generate_id(c);
      x.xcb_create_gc(c, gc, window, 0, nullptr);
      // Ours alone on the server, as for the light: other windows mapping or restacking over it.
      const uint32_t rootEvents = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
      x.xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &rootEvents);
      x.xcb_flush(c);
      if (x.xcb_connection_has_error(c))
      {
         why = "the X connection failed while creating the banner";
         return false;
      }
      return true;
   }

   void SetText(const BannerText& banner)
   {
      if (banner == text)
         return;
      text = banner;
      laidOut = false;
   }

   // Every tick while an alert is up: placed in the playfield and raised over it while VPX is in
   // front, hidden otherwise. False once the connection has failed.
   bool Follow()
   {
      if (x.xcb_connection_has_error(c))
         return false;
      Events();
      const xcb_window_t playfield = FindPlayfield();
      Rect area;
      if (playfield == XCB_WINDOW_NONE || !WindowRect(playfield, area) || area.w <= 0 || area.h <= 0)
      {
         Unmap();
         x.xcb_flush(c);
         return true;
      }
      if (!laidOut || area.w != laidOutWidth)
         Layout(area.w);
      const Rect at { area.x + (area.w - size.w) / 2, area.y + static_cast<int>(std::lround(BANNER_TOP * area.h)), size.w, size.h };
      if (!(at == placed))
      {
         const uint32_t geometry[] = { static_cast<uint32_t>(at.x), static_cast<uint32_t>(at.y), static_cast<uint32_t>(at.w),
            static_cast<uint32_t>(at.h) };
         x.xcb_configure_window(c, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            geometry);
         if (at.w != placed.w || at.h != placed.h)
            Shape(at.w, at.h);
         placed = at;
         dirty = true;
      }
      if (!mapped)
      {
         // Stacked first: a window maps at the top, over any override-redirect window.
         StackAbove();
         x.xcb_map_window(c, window);
         mapped = true;
         dirty = true;
      }
      else if (raise && Clock::now() - lastRaise >= RAISE_GAP)
         StackAbove();
      if (dirty)
         Draw();
      x.xcb_flush(c);
      return !x.xcb_connection_has_error(c);
   }

   // Every tick while no alert is up: the root's events are still read, or the server queues them.
   void Idle()
   {
      Events();
      Unmap();
      x.xcb_flush(c);
   }

   void Unmap()
   {
      if (mapped)
         x.xcb_unmap_window(c, window);
      mapped = false;
   }

private:
   // A scalable core font at the banner's size, bold where there is one. A bitmap font scaled that
   // far is unreadable, and "fixed" cannot be scaled at all: with neither, VPX's notice is used.
   bool OpenFonts(std::string& why)
   {
      static constexpr const char* PATTERNS[] = {
         "-*-helvetica-bold-r-normal--0-0-0-0-p-0-iso8859-1",
         "-*-*-bold-r-normal-*-0-0-0-0-p-0-iso8859-1",
         "-*-*-medium-r-normal-*-0-0-0-0-p-0-iso8859-1",
      };
      std::string scalable;
      for (const char* pattern : PATTERNS)
         if (xcb_list_fonts_reply_t* list = x.xcb_list_fonts_reply(c,
                x.xcb_list_fonts(c, 1, static_cast<uint16_t>(std::strlen(pattern)), pattern), nullptr))
         {
            auto it = x.xcb_list_fonts_names_iterator(list);
            if (it.rem > 0)
               scalable.assign(x.xcb_str_name(it.data), static_cast<size_t>(x.xcb_str_name_length(it.data)));
            std::free(list);
            if (!scalable.empty())
               break;
         }
      if (scalable.empty())
      {
         why = "the X server has no scalable font";
         return false;
      }
      // The XLFD's size fields (pixel, point, x and y resolution, spacing, average width) at this size.
      size_t field = 0, at = 0;
      for (int dashes = 0; dashes < 7 && at != std::string::npos; dashes++)
         at = scalable.find('-', at + (dashes > 0 ? 1 : 0));
      field = at;
      const size_t tail = field == std::string::npos ? std::string::npos : scalable.find('-', field + 1);
      size_t charset = tail;
      for (int dashes = 0; dashes < 5 && charset != std::string::npos; dashes++)
         charset = scalable.find('-', charset + 1);
      if (field == std::string::npos || charset == std::string::npos)
      {
         why = "the X server's scalable font has an unreadable name";
         return false;
      }
      const auto sized = [&](int px) { return scalable.substr(0, field + 1) + std::to_string(px) + "-*-*-*-*-*" + scalable.substr(charset); };
      font = OpenFont(sized(textPx), fontAscent, fontDescent);
      if (font == 0)
      {
         why = "the X server's scalable font did not open (" + scalable + ")";
         return false;
      }
      line = std::max(1, fontAscent + fontDescent);
      // The icon's "!", "i" or "?", at about the size the other platforms draw it.
      glyphFont = OpenFont(sized(std::max(1, static_cast<int>(std::lround(line * 0.6)))), glyphAscent, glyphDescent);
      if (glyphFont == 0)
      {
         glyphFont = font;
         glyphAscent = fontAscent;
         glyphDescent = fontDescent;
      }
      return true;
   }

   // A font's id, or 0 when the server could not open it: an error on the open, which a query shows.
   xcb_font_t OpenFont(const std::string& name, int& ascent, int& descent)
   {
      const xcb_font_t f = x.xcb_generate_id(c);
      x.xcb_open_font(c, f, static_cast<uint16_t>(name.size()), name.c_str());
      int width = 0;
      return TextExtents(f, "x", ascent, descent, width) ? f : 0;
   }

   uint32_t Pixel(uint32_t rgb) const { return screen->root_depth == 32 ? 0xff000000u | rgb : rgb; }

   // Sizes from docs/status-banner.md, "Size": wrapped to 60% of the playfield's width, three lines
   // at most, the last ending in "..." when the text is longer (Latin-1 has no ellipsis).
   void Layout(int areaWidth)
   {
      pad = (textPx + 1) / 2;
      icon = line;
      const int maxText = std::max(1, static_cast<int>(BANNER_MAX_WIDTH * areaWidth) - 3 * pad - icon);
      lines = Wrap(font, Latin1(text.text), maxText);
      if (lines.size() > static_cast<size_t>(BANNER_MAX_LINES))
      {
         lines.resize(BANNER_MAX_LINES);
         std::string& last = lines.back();
         while (!last.empty() && !Fits(font, last + "...", maxText))
         {
            const size_t space = last.find_last_of(' ');
            last.erase(space != std::string::npos && space > 0 ? space : last.size() - 1);
         }
         while (!last.empty() && last.back() == ' ')
            last.pop_back();
         last += "...";
      }
      int widest = 1, ascent = 0, descent = 0, width = 0;
      for (const std::string& l : lines)
         if (TextExtents(font, l, ascent, descent, width))
            widest = std::max(widest, std::min(width, maxText));
      textHeight = static_cast<int>(lines.size()) * line;
      const int inner = std::max(textHeight, icon);
      size = { 0, 0, pad + icon + pad + widest + pad, pad + inner + pad };
      laidOut = true;
      laidOutWidth = areaWidth;
      dirty = true;
   }

   // The rounded corners, as a bounding shape: the server draws the box itself, with no alpha.
   void Shape(int w, int h)
   {
      const int r = std::max(1, pad / 2);
      std::vector<xcb_rectangle_t> runs;
      for (int y = 0; y < h; y++)
      {
         const int cy = y < r ? r - 1 - y : y >= h - r ? y - (h - r) : -1;
         int inset = 0;
         if (cy >= 0)
            while (inset < r && (r - inset) * (r - inset) + (cy + 1) * (cy + 1) > r * r)
               inset++;
         runs.push_back({ static_cast<int16_t>(inset), static_cast<int16_t>(y), static_cast<uint16_t>(std::max(1, w - 2 * inset)), 1 });
      }
      x.xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_YX_BANDED, window, 0, 0,
         static_cast<uint32_t>(runs.size()), runs.data());
   }

   void Draw()
   {
      dirty = false;
      const uint32_t black[] = { Pixel(BANNER_BLACK) };
      x.xcb_change_gc(c, gc, XCB_GC_FOREGROUND, black);
      const xcb_rectangle_t whole { 0, 0, static_cast<uint16_t>(placed.w), static_cast<uint16_t>(placed.h) };
      x.xcb_poly_fill_rectangle(c, window, gc, 1, &whole);

      // The icon, level with the first line; its glyph is cut out of it in black.
      const bool triangle = text.icon == BannerIcon::Problem;
      const uint32_t shape[] = { Pixel(triangle ? BANNER_YELLOW : BANNER_WHITE) };
      x.xcb_change_gc(c, gc, XCB_GC_FOREGROUND, shape);
      if (triangle)
      {
         const xcb_point_t points[] = { { static_cast<int16_t>(pad + icon / 2), static_cast<int16_t>(pad) },
            { static_cast<int16_t>(pad + icon), static_cast<int16_t>(pad + icon) }, { static_cast<int16_t>(pad), static_cast<int16_t>(pad + icon) } };
         x.xcb_fill_poly(c, window, gc, XCB_POLY_SHAPE_CONVEX, XCB_COORD_MODE_ORIGIN, 3, points);
      }
      else
      {
         const xcb_arc_t disc { static_cast<int16_t>(pad), static_cast<int16_t>(pad), static_cast<uint16_t>(icon), static_cast<uint16_t>(icon), 0, 360 * 64 };
         x.xcb_poly_fill_arc(c, window, gc, 1, &disc);
      }
      const char glyph = triangle ? '!' : text.icon == BannerIcon::Unknown ? '?' : 'i';
      int ascent = 0, descent = 0, width = 0;
      TextExtents(glyphFont, std::string(1, glyph), ascent, descent, width);
      // A triangle's middle sits low; its glyph follows it.
      const int shift = triangle ? static_cast<int>(std::lround(icon * 0.14)) : 0;
      const int gx = pad + (icon - width) / 2;
      const int gy = pad + shift + (icon + glyphAscent - glyphDescent) / 2;
      const uint32_t ink[] = { Pixel(BANNER_BLACK), glyphFont };
      x.xcb_change_gc(c, gc, XCB_GC_FOREGROUND | XCB_GC_FONT, ink);
      const uint8_t item[] = { 1, 0, static_cast<uint8_t>(glyph) };  // one TEXTITEM8: length, delta, the character
      x.xcb_poly_text_8(c, window, gc, static_cast<int16_t>(gx), static_cast<int16_t>(gy), sizeof(item), item);

      const uint32_t textInk[] = { Pixel(BANNER_YELLOW), Pixel(BANNER_BLACK), font };
      x.xcb_change_gc(c, gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT, textInk);
      const int inner = std::max(textHeight, icon);
      int baseline = pad + (inner - textHeight) / 2 + fontAscent;
      for (const std::string& l : lines)
      {
         x.xcb_image_text_8(c, static_cast<uint8_t>(std::min<size_t>(l.size(), 255)), window, gc, static_cast<int16_t>(pad + icon + pad),
            static_cast<int16_t>(baseline), l.c_str());
         baseline += line;
      }
   }

   void Events()
   {
      while (xcb_generic_event_t* event = x.xcb_poll_for_event(c))
      {
         switch (event->response_type & 0x7f)
         {
         case XCB_EXPOSE:
            dirty = true;
            break;
         // Another managed window moved or was restacked, perhaps over the banner.
         case XCB_CONFIGURE_NOTIFY:
         {
            const auto* configure = reinterpret_cast<xcb_configure_notify_event_t*>(event);
            if (configure->window != window && !configure->override_redirect)
               raise = true;
            break;
         }
         case XCB_MAP_NOTIFY:
         {
            const auto* map = reinterpret_cast<xcb_map_notify_event_t*>(event);
            if (map->window != window && !map->override_redirect)
               raise = true;
            break;
         }
         default:
            break;
         }
         std::free(event);
      }
   }

   void StackAbove()
   {
      lastRaise = Clock::now();
      raise = false;
      const xcb_window_t sibling = TopManaged();
      if (sibling == XCB_WINDOW_NONE)
         return;
      const uint32_t values[] = { sibling, XCB_STACK_MODE_ABOVE };
      x.xcb_configure_window(c, window, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
   }

   xcb_window_t window = XCB_WINDOW_NONE;
   xcb_gcontext_t gc = 0;
   xcb_font_t font = 0, glyphFont = 0;
   int fontAscent = 0, fontDescent = 0, glyphAscent = 0, glyphDescent = 0;
   double scale = 1;
   int textPx = 1, line = 1, pad = 1, icon = 1, textHeight = 1;
   BannerText text;
   std::vector<std::string> lines;
   bool laidOut = false;
   int laidOutWidth = 0;
   Rect size, placed;
   bool mapped = false, dirty = true, raise = false;
   Clock::time_point lastRaise;
};

Light* light = nullptr;

// VPX draws through SDL, and only SDL's x11 driver puts its playfield where an X window can sit
// over it; VPX asks SDL the same question (Window.cpp). SDL is in the process before any plugin.
const char* DriverProblem()
{
   static std::string why;
   using Driver = const char* (*)();
   const auto driver = reinterpret_cast<Driver>(dlsym(RTLD_DEFAULT, "SDL_GetCurrentVideoDriver"));
   if (const char* name = driver != nullptr ? driver() : nullptr)
   {
      if (std::string_view(name) == "x11")
         return nullptr;
      why = "VPX draws with SDL's " + std::string(name) + " video driver, not x11";
      return why.c_str();
   }
   // SDL not found by name: guess as SDL would choose, Wayland first unless told otherwise. SDL 3
   // reads SDL_VIDEO_DRIVER, SDL 2 SDL_VIDEODRIVER; either may list drivers to try in order.
   for (const char* variable : { "SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER" })
      if (const char* forced = std::getenv(variable); forced != nullptr && *forced != '\0')
      {
         const std::string_view list(forced);
         if (list.substr(0, list.find(',')) == "x11")
            return nullptr;
         why = std::string(variable) + " is " + forced;
         return why.c_str();
      }
   if (const char* wayland = std::getenv("WAYLAND_DISPLAY"); wayland != nullptr && *wayland != '\0')
      return "a Wayland session";
   if (const char* display = std::getenv("DISPLAY"); display == nullptr || *display == '\0')
      return "no X display";
   return nullptr;
}

void Hide()
{
   if (worker.joinable())
   {
      stopping.store(true, std::memory_order_release);
      worker.join();
   }
   if (light != nullptr)
      light->Close();
   delete light;
   light = nullptr;
}

// If the library goes (dlclose, process exit) without the plugin hiding the light, a joinable
// std::thread destroyed with the statics calls std::terminate. Defined after them, so destroyed first.
// The banner, the API thread's alone. Once it can't be made, VPX's notice is used for the plugin's life.
Banner* banner = nullptr;
std::string bannerUnavailable;
bool bannerUp = false;
Clock::time_point bannerUntil;

void CloseBanner()
{
   if (banner != nullptr)
      banner->Close();
   delete banner;
   banner = nullptr;
   bannerUp = false;
}

bool MakeBanner()
{
   if (banner != nullptr)
      return true;
   if (!bannerUnavailable.empty())
      return false;
   if (const char* why = DriverProblem())
   {
      bannerUnavailable = why;
      return false;
   }
   banner = new Banner();
   if (!banner->Open(bannerUnavailable))
   {
      if (bannerUnavailable.empty())
         bannerUnavailable = "the banner could not be made";
      CloseBanner();
      return false;
   }
   return true;
}

// Placed and raised while up, hidden otherwise. False when its connection failed: then it is gone.
bool FollowBanner()
{
   if (banner == nullptr)
      return false;
   if (!bannerUp)
   {
      banner->Idle();
      return true;
   }
   if (banner->Follow())
      return true;
   bannerUnavailable = "the X connection closed";
   CloseBanner();
   return false;
}

struct StopAtUnload
{
   ~StopAtUnload()
   {
      Hide();
      CloseBanner();
   }
} stopAtUnload;

bool Show()
{
   if (const char* why = DriverProblem())
   {
      unavailable = why;
      return false;
   }
   light = new Light();
   if (!light->Open(unavailable))
   {
      Hide();
      return false;
   }
   unavailable.clear();
   stopping.store(false, std::memory_order_relaxed);
   lost.store(false, std::memory_order_relaxed);
   worker = std::thread([] { light->Run(); });
   return true;
}

}

bool UpdateStatusWindow(int corner, const LightView& view, double opacity, bool flash, const std::string& caption)
{
   if (corner < 1 || corner > 4 || !view.visible)
   {
      if (light != nullptr)
         Hide();
      unavailable.clear();
      return false;
   }
   if (light != nullptr && lost.load(std::memory_order_acquire))
   {
      Hide();
      unavailable = "the X connection closed";
   }
   // One attempt a game: a failure stands until the light is next hidden, as at the game's end.
   if (light == nullptr && !unavailable.empty())
      return false;
   const bool appeared = light == nullptr;
   {
      // Written before a new light's thread starts, so its first frame is this one.
      std::lock_guard guard(wantedLock);
      const auto now = Clock::now();
      if (appeared || !(view == wanted.view))
      {
         wanted.view = view;
         wanted.since = now;
      }
      if (appeared)
         wanted.flash = false;
      if (flash)
      {
         wanted.flash = true;
         wanted.flashAt = now;
      }
      wanted.corner = corner;
      wanted.alpha = static_cast<uint8_t>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255));
      wanted.caption = caption;
   }
   if (appeared && !Show())
      return false;
   return appeared;
}

const char* StatusWindowUnavailable()
{
   return unavailable.empty() ? nullptr : unavailable.c_str();
}

bool StatusBannerSupported()
{
   // Made here, so a banner that can't be made never drops a state VPX's notice would show.
   return MakeBanner();
}

bool ShowStatusBanner(const BannerText& text, double lifetimeSeconds)
{
   if (!MakeBanner())
      return false;
   banner->SetText(text);
   bannerUp = true;
   bannerUntil = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(lifetimeSeconds * 1000));
   // A banner that could not be drawn hands the alert to VPX's notice.
   return FollowBanner();
}

void HideStatusBanner()
{
   bannerUp = false;
   FollowBanner();
}

void TickStatusBanner(bool release)
{
   if (release)
   {
      CloseBanner();
      return;
   }
   if (bannerUp && Clock::now() >= bannerUntil)
      bannerUp = false;
   FollowBanner();
}

const char* StatusBannerUnavailable()
{
   return bannerUnavailable.empty() ? nullptr : bannerUnavailable.c_str();
}

}
