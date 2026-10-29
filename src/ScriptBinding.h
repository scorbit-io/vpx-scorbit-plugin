// license:GPLv3+

#pragma once

// The Scorbit object for table scripts (SB-5990). An authored table writes
//
//    Set Scorbit = CreateObject("Scorbit.Table")
//    Scorbit.Declare "<machine id>"
//
// and VPX hands that CreateObject to this plugin instead of COM. Without the plugin the
// CreateObject fails, so a script must guard it (SB-6010's author guidance).
//
// Only Declare exists so far, held host-side; the game calls arrive with SB-5984's layouts.

#include "plugins/MsgPlugin.h"

#include <cstdint>

namespace Scorbit
{

class VpxSessionSource;

// API thread, once each at plugin load and unload. Without the scriptable API (an older
// VPX), registration logs once and the plugin runs on without the object. At quit a
// script object may outlive the unload and still point at the class, so then the class
// is left registered for the process to end with, and objects only stop forwarding.
void RegisterScriptBinding(const MsgPluginAPI* msgApi, uint32_t endpointId, VpxSessionSource* session);
void UnregisterScriptBinding(bool atQuit);

}
