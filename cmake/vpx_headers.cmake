# The plugin compiles against Visual Pinball's plugin API, which is header-only
# and lives in the vpinball and pinmame repositories. The headers are fetched at
# configure time from pinned commits and verified by hash, so a build is
# reproducible and the API the plugin was built against is explicit.
#
# To move to a newer VPX: update the commit, then update the hashes (the
# configure step prints the actual hash when one no longer matches).

set(VPX_PLUGIN_API_COMMIT "24af13723acc7c5a6ccbaa2762af2c9458adf293") # vpinball/vpinball master, 2026-09-02 (call-context plugin API; needs a VPX build at or after this)
set(PINMAME_PLUGIN_API_COMMIT "6589fc5734853e00c0412d415f4e29af2dc475af") # vpinball/pinmame master, 2026-09-02

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
_fetch_header("${_vpx_raw}/MsgPlugin.h"        "${VPX_HEADERS_DIR}/plugins/MsgPlugin.h"        "df7f3e9534cf745b3c5e1742a05be9a44c760b18488ba0ff4cbf89bb868835b3")
_fetch_header("${_vpx_raw}/ControllerPlugin.h" "${VPX_HEADERS_DIR}/plugins/ControllerPlugin.h" "4c67f8a1fb921515366dc273a7834a7652c0f8fe057483e4f2e4472f08ae2ca8")
_fetch_header("${_vpx_raw}/VPXPlugin.h"        "${VPX_HEADERS_DIR}/plugins/VPXPlugin.h"        "30d8e4b409d016c4b6be5c3503ecc27be06ee5cfbec23df45c30d16834909db0")
_fetch_header("${_vpx_raw}/LoggingPlugin.h"    "${VPX_HEADERS_DIR}/plugins/LoggingPlugin.h"    "d73bd1cbb5264dd537a7f1b86668fff2bd42e9dfbb0f42406e77227b0ca9f363")

set(_pinmame_raw "https://raw.githubusercontent.com/vpinball/pinmame/${PINMAME_PLUGIN_API_COMMIT}/src/libpinmame")
_fetch_header("${_pinmame_raw}/PinMAMEPlugin.h" "${VPX_HEADERS_DIR}/pinmame/PinMAMEPlugin.h" "6d870ed577f121a2ff8acc0468a4449016d8b01661c495fc171b01805a74b0a3")
