# The plugin compiles against Visual Pinball's plugin API, which is header-only
# and lives in the vpinball and pinmame repositories. The headers are fetched at
# configure time from pinned commits and verified by hash, so a build is
# reproducible and the API the plugin was built against is explicit.
#
# To move to a newer VPX: update the commit, then update the hashes (the
# configure step prints the actual hash when one no longer matches).

set(VPX_PLUGIN_API_COMMIT "6fd599d794fc27982cc56f9e9ba419e3d0ea0030") # vpinball/vpinball, "ControllerPlugin: refactor game states", 2026-08-24
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
_fetch_header("${_vpx_raw}/ControllerPlugin.h" "${VPX_HEADERS_DIR}/plugins/ControllerPlugin.h" "06a36a2b27bf061a35c9073c4be669cae65b5f99b4b07dd7631cef53f4271235")
_fetch_header("${_vpx_raw}/VPXPlugin.h"        "${VPX_HEADERS_DIR}/plugins/VPXPlugin.h"        "7fa27df5e6fdc3a78ebcd900dadbb4698b7f8cc5c051e3083eecbe364e8f1902")
_fetch_header("${_vpx_raw}/LoggingPlugin.h"    "${VPX_HEADERS_DIR}/plugins/LoggingPlugin.h"    "d73bd1cbb5264dd537a7f1b86668fff2bd42e9dfbb0f42406e77227b0ca9f363")

set(_pinmame_raw "https://raw.githubusercontent.com/vpinball/pinmame/${PINMAME_PLUGIN_API_COMMIT}/src/libpinmame")
_fetch_header("${_pinmame_raw}/PinMAMEPlugin.h" "${VPX_HEADERS_DIR}/pinmame/PinMAMEPlugin.h" "6d870ed577f121a2ff8acc0468a4449016d8b01661c495fc171b01805a74b0a3")
