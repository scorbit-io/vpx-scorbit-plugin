// license:GPLv3+

#pragma once

// Messages the Scorbit plugin serves on the VPX plugin message bus.
//
// This is the in-process seam between the plugin and whatever drives it: today
// a test broadcaster, later the socket worker that talks to the Scorbit daemon.
// All messages must be sent on the API thread (RunOnMainThread with a zero,
// never negative, delay from a worker).

#include <stdint.h>

#define SCORBIT_NAMESPACE "Scorbit"

// Drive the DMD overlay. Mirrors the hardware probe sequence exactly: upload a
// bitmap (WriteDmdOverlay, 0x23), show it for a duration or until hidden
// (WriteDmdOverlayControlReg, 0x27), hide it. Message data is a
// ScorbitOverlayMsg.
// The ":1" suffix follows the version-marker convention documented in MsgPlugin.h: a
// message whose format or behaviour changes must change its marker, so a mismatched
// peer subscribes to a name nobody serves and sees nothing rather than misreading data.
// :2 (SB-5699) changed upload behaviour only, not the struct: see the payload comment.
// :3 changed behaviour again, as the probe firmware does it: trailing upload
// bytes are ignored, Show arms before any upload, every op is refused without a display,
// and a value is shaded per platform rather than as a brightness.
#define SCORBIT_MSG_OVERLAY "Overlay:3"

#define SCORBIT_OVERLAY_OP_UPLOAD 1
#define SCORBIT_OVERLAY_OP_SHOW   2
#define SCORBIT_OVERLAY_OP_HIDE   3

typedef struct ScorbitOverlayMsg
{
   int version;            // Always 1: the layout, unchanged by the :2 and :3 markers
   int op;                 // SCORBIT_OVERLAY_OP_xxx
   // Upload: the WriteDmdOverlay payload as the daemon builds it. Header is two
   // bytes, width then height, followed by width*height pixels, row major, one
   // byte each: 0x80 set for transparent, the low seven bits the platform's
   // bitplane bits, shaded as the probe on that platform shows them. Bytes past
   // the pixels are ignored. As the probe firmware does: the bitmap is centred on
   // the display, and a larger one is cropped. Outside 10x10 to 192x64 it is
   // refused and the previous one kept.
   const uint8_t* payload;
   uint32_t payloadSize;
   // Show: milliseconds to stay visible, 0 for until hidden. Arms the overlay even
   // before an upload, which then shows at once if it arrives inside the window.
   uint32_t durationMs;
   // Response: 1 if accepted, 0 if refused (no display, bad payload, size out of bounds)
   int applied;
} ScorbitOverlayMsg;
