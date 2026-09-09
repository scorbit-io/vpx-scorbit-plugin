# The plugin compiles against Visual Pinball's plugin API, which is header-only
# and lives in the vpinball and pinmame repositories. The headers are fetched at
# configure time from pinned commits and verified by hash, so a build is
# reproducible and the API the plugin was built against is explicit.
#
# To move to a newer VPX: update the commit, then update the hashes (the
# configure step prints the actual hash when one no longer matches).

set(VPX_PLUGIN_API_COMMIT "0bc9838ed5f1bbac869efdfb6e785b829a541d3f") # vpinball/vpinball master, 2026-09-09 (versioned message names; the commit VPinballX_BGFX-5589 was built from)
set(PINMAME_PLUGIN_API_COMMIT "6a673a2375bf82d1998e92caf7a598f850a94a44") # vpinball/pinmame master, 2026-09-09

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
_fetch_header("${_vpx_raw}/MsgPlugin.h"        "${VPX_HEADERS_DIR}/plugins/MsgPlugin.h"        "f8cb28f72c9a482a74b7330766a87cf20b87dc97240664662f92cd318cff9775")
_fetch_header("${_vpx_raw}/ControllerPlugin.h" "${VPX_HEADERS_DIR}/plugins/ControllerPlugin.h" "7d0b5376545ef1f71117b6d6953960a2260aef473802f056983b3e4bc903c1ea")
_fetch_header("${_vpx_raw}/VPXPlugin.h"        "${VPX_HEADERS_DIR}/plugins/VPXPlugin.h"        "20359f8fec1eb045c9fef38106b885ec5c334a9de56e3814d2fa8cdd937ed30a")
_fetch_header("${_vpx_raw}/LoggingPlugin.h"    "${VPX_HEADERS_DIR}/plugins/LoggingPlugin.h"    "46e98350f86e34936cc250824421de366e5dce5ecd1636ba904faddae537f4b0")

set(_pinmame_raw "https://raw.githubusercontent.com/vpinball/pinmame/${PINMAME_PLUGIN_API_COMMIT}/src/libpinmame")
_fetch_header("${_pinmame_raw}/PinMAMEPlugin.h" "${VPX_HEADERS_DIR}/pinmame/PinMAMEPlugin.h" "e6c9f436bc5277bdb9d32e13710149873b66b41f4b5f5f099faae078da89b6d2")
