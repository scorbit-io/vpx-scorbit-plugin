# The plugin compiles against Visual Pinball's plugin API, which is header-only
# and lives in the vpinball and pinmame repositories. The headers are fetched at
# configure time from pinned commits and verified by hash, so a build is
# reproducible and the API the plugin was built against is explicit.
#
# To move to a newer VPX: update the commit, then update the hashes (the
# configure step prints the actual hash when one no longer matches).

set(VPX_PLUGIN_API_COMMIT "9a8f24cb6e6faa1bf0d072bfbb81702d06dc0a9e") # vpinball/vpinball master (the commit VPinballX_BGFX-6461 was built from; #4228 UpdateNotification returns the handle)
set(PINMAME_PLUGIN_API_COMMIT "5fc234819994301ccaa1a04e8f22456119a5a417") # vpinball/pinmame master (the PINMAME_SHA VPX 6461 builds; WriteMemory answers 0 for an op outside the address space)

# The build the headers above come from. The plugin API exposes no runtime
# version, so this is what the plugin reports to the Scorbit daemon in Hello and
# Declare: the VPX it was compiled against, not the VPX it was loaded into.
set(VPX_PINNED_VERSION "VPinballX_BGFX-6461")
set(VPX_PINNED_REVISION "6461")

set(VPX_HEADERS_DIR "${CMAKE_BINARY_DIR}/vpx-headers")

function(_fetch_header url dest sha256)
   if(EXISTS "${dest}")
      file(SHA256 "${dest}" existing)
      if(existing STREQUAL sha256)
         return()
      endif()
   endif()
   message(STATUS "Fetching ${url}")
   file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} STATUS status)
   list(GET status 0 code)
   if(NOT code EQUAL 0)
      list(GET status 1 msg)
      message(FATAL_ERROR "Failed to fetch ${url}: ${msg}")
   endif()
endfunction()

set(_vpx_raw "https://raw.githubusercontent.com/vpinball/vpinball/${VPX_PLUGIN_API_COMMIT}/plugins/plugins")
_fetch_header("${_vpx_raw}/MsgPlugin.h"        "${VPX_HEADERS_DIR}/plugins/MsgPlugin.h"        "cdf932ac0b65c321be28eca24407a04c3009ae494806c6e30853fc347df61de3")
_fetch_header("${_vpx_raw}/ControllerPlugin.h" "${VPX_HEADERS_DIR}/plugins/ControllerPlugin.h" "b9b48930d6ed63844c5e341aad91d3c8cff01f02ee45dfbba2a45c0c9bdf3f53")
_fetch_header("${_vpx_raw}/VPXPlugin.h"        "${VPX_HEADERS_DIR}/plugins/VPXPlugin.h"        "7f821c5f309c554e42db69c3afb295deed77a1dcc4579627dc38df20ca6cad19")
_fetch_header("${_vpx_raw}/LoggingPlugin.h"    "${VPX_HEADERS_DIR}/plugins/LoggingPlugin.h"    "46e98350f86e34936cc250824421de366e5dce5ecd1636ba904faddae537f4b0")

set(_pinmame_raw "https://raw.githubusercontent.com/vpinball/pinmame/${PINMAME_PLUGIN_API_COMMIT}/src/libpinmame")
_fetch_header("${_pinmame_raw}/PinMAMEPlugin.h" "${VPX_HEADERS_DIR}/pinmame/PinMAMEPlugin.h" "69a0b80b421bcddabbaa20521b4b70e25e22ddeff1611fa18828f8e2b9b4bcf5")
