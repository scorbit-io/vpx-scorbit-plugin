# The plugin compiles against Visual Pinball's plugin API, which is header-only
# and lives in the vpinball and pinmame repositories. The headers are fetched at
# configure time from pinned commits and verified by hash, so a build is
# reproducible and the API the plugin was built against is explicit.
#
# To move to a newer VPX: update the commit, then update the hashes (the
# configure step prints the actual hash when one no longer matches).

set(VPX_PLUGIN_API_COMMIT "84effd7d3e75612652aa3ed68567e47c29cb9a98") # vpinball/vpinball master, 2026-10-17 (the commit VPinballX_BGFX-6289 was built from)
set(PINMAME_PLUGIN_API_COMMIT "3e85c5c3fa7d0b11147f590f136b03386d055129") # vpinball/pinmame master, 2026-10-15 (#634 WriteMemory; the PINMAME_SHA VPX 6289 builds)

# The build the headers above come from. The plugin API exposes no runtime
# version, so this is what the plugin reports to the Scorbit daemon in Hello and
# Declare: the VPX it was compiled against, not the VPX it was loaded into.
set(VPX_PINNED_VERSION "VPinballX_BGFX-6289")
set(VPX_PINNED_REVISION "6289")

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
_fetch_header("${_vpx_raw}/VPXPlugin.h"        "${VPX_HEADERS_DIR}/plugins/VPXPlugin.h"        "381cef59f592c8f6e067756f176450bf5ddb1926af3f8e2513e82fb3c324ecca")
_fetch_header("${_vpx_raw}/LoggingPlugin.h"    "${VPX_HEADERS_DIR}/plugins/LoggingPlugin.h"    "46e98350f86e34936cc250824421de366e5dce5ecd1636ba904faddae537f4b0")

set(_pinmame_raw "https://raw.githubusercontent.com/vpinball/pinmame/${PINMAME_PLUGIN_API_COMMIT}/src/libpinmame")
_fetch_header("${_pinmame_raw}/PinMAMEPlugin.h" "${VPX_HEADERS_DIR}/pinmame/PinMAMEPlugin.h" "f4896a52a371b6d942fdd76598c47f4700f706c34c304f2d0479daaaed87e035")
