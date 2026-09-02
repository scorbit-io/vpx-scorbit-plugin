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
#define SCORBIT_MSG_OVERLAY "Overlay"

#define SCORBIT_OVERLAY_OP_UPLOAD 1
#define SCORBIT_OVERLAY_OP_SHOW   2
#define SCORBIT_OVERLAY_OP_HIDE   3

typedef struct ScorbitOverlayMsg
{
   int version;            // Always 1
   int op;                 // SCORBIT_OVERLAY_OP_xxx
   // Upload: the WriteDmdOverlay payload as the daemon builds it. Header is two
   // bytes, width then height, followed by width*height pixels, row major, one
   // byte each: brightness 0-15 in the low bits, 0x80 set for transparent.
   // The bitmap is centred on the display and must not exceed it.
   const uint8_t* payload;
   uint32_t payloadSize;
   // Show: milliseconds to stay visible, 0 for until hidden.
   uint32_t durationMs;
   // Response: 1 if applied, 0 if refused (no display, bad payload)
   int applied;
} ScorbitOverlayMsg;
