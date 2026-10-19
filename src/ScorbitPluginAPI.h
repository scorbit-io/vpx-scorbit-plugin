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
#define SCORBIT_MSG_OVERLAY "Overlay:2"

#define SCORBIT_OVERLAY_OP_UPLOAD 1
#define SCORBIT_OVERLAY_OP_SHOW   2
#define SCORBIT_OVERLAY_OP_HIDE   3

typedef struct ScorbitOverlayMsg
{
   int version;            // Always 1: the layout, unchanged by the :2 marker
   int op;                 // SCORBIT_OVERLAY_OP_xxx
   // Upload: the WriteDmdOverlay payload as the daemon builds it. Header is two
   // bytes, width then height, followed by width*height pixels, row major, one
   // byte each: brightness 0-15 in the low bits, 0x80 set for transparent.
   // As the probe firmware does: the bitmap is centred on the display, and a larger
   // one is cropped. Outside 10x10 to 192x64 it is refused and the previous one kept.
   const uint8_t* payload;
   uint32_t payloadSize;
   // Show: milliseconds to stay visible, 0 for until hidden.
   uint32_t durationMs;
   // Response: 1 if applied, 0 if refused (no display, bad payload, size out of bounds)
   int applied;
} ScorbitOverlayMsg;
