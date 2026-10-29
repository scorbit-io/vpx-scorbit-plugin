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
// VPX), registration logs once and the plugin runs on without the object.
void RegisterScriptBinding(const MsgPluginAPI* msgApi, uint32_t endpointId, VpxSessionSource* session);
void UnregisterScriptBinding();

}
