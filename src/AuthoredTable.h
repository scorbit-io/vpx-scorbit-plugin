// license:GPLv3+

#pragma once

// What an authored table declares through the Scorbit script object (SB-5990), and the
// paths the daemon needs to find the table's script. Host-only: the paths name the user's
// own folders, so they never go into a log line, and WireDump redacts them on the wire.
// Not sent yet: the authored Declare's layout is the daemon's to define first (SB-5984).

#include <string>

namespace Scorbit
{

struct AuthoredTable
{
   std::string machineId; // from Scorbit.Declare; empty until the script calls it
   std::string tablePath; // the table file, full path, UTF-8
   std::string vpxPath;   // VPX's application folder, UTF-8
   std::string prefPath;  // VPX's preferences folder, UTF-8

   bool operator==(const AuthoredTable&) const = default;
};

// A machine id as Scorbit.Declare accepts it: 1 to 64 printable ASCII characters, no spaces.
bool ValidMachineId(const std::string& id);

// VPX hands paths over native narrow (VPXPlugin.h): the process code page on Windows, UTF-8
// elsewhere. Returns UTF-8, or empty for null. A character the code page lacks is already
// '_' by then, and that cannot be undone here.
std::string NativePathToUtf8(const char* path);

}
