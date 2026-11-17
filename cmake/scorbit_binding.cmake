# The VBScript binding a table loads with GetTextFile("scorbit-<version>.vbs"), from a pinned
# scorbit_sdk release, verified by hash as the VPX headers are. It is staged beside the plugin and
# added to VPX's <prefPath>/scripts at load (src/BindingInstall.h, docs/vbs-sdk.md).
#
# To move to a newer release: update the version, then the hash, which the release publishes as
# scorbit-<version>.vbs.sha256 (the configure step also prints the actual hash on a mismatch).
# Never re-pin to different bytes under the same version: installed copies are never replaced.

set(SCORBIT_BINDING_VERSION "2.0.8")
set(SCORBIT_BINDING_SHA256 "99e2cd49af98faa443b000c2341a6a0a4a996e1757cbb7ea7bee5a55294540e6")

set(SCORBIT_BINDING_FILE "scorbit-${SCORBIT_BINDING_VERSION}.vbs")
set(SCORBIT_BINDING_PATH "${CMAKE_BINARY_DIR}/scorbit-binding/${SCORBIT_BINDING_FILE}")

set(_binding_url "https://github.com/scorbit-io/scorbit_sdk/releases/download/${SCORBIT_BINDING_VERSION}/${SCORBIT_BINDING_FILE}")
set(_binding_have FALSE)
if(EXISTS "${SCORBIT_BINDING_PATH}")
   file(SHA256 "${SCORBIT_BINDING_PATH}" _binding_existing)
   if(_binding_existing STREQUAL SCORBIT_BINDING_SHA256)
      set(_binding_have TRUE)
   endif()
endif()
if(NOT _binding_have)
   message(STATUS "Fetching ${_binding_url}")
   file(DOWNLOAD "${_binding_url}" "${SCORBIT_BINDING_PATH}" EXPECTED_HASH SHA256=${SCORBIT_BINDING_SHA256} STATUS _binding_status)
   list(GET _binding_status 0 _binding_code)
   if(NOT _binding_code EQUAL 0)
      list(GET _binding_status 1 _binding_msg)
      message(FATAL_ERROR "Failed to fetch ${_binding_url}: ${_binding_msg}")
   endif()
endif()
