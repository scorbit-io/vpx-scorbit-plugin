cmake_minimum_required(VERSION 3.28)

project(vpx-scorbit-plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Where a prebuilt Scorbit SDK lives (a directory containing include/ and the
# library). Empty means build the SDK from source through CPM.
set(SCORBIT_SDK_ROOT "" CACHE PATH "Prebuilt scorbit_sdk root; empty builds it from source")
# Pinned to a main commit rather than the 2.0.4 tag: 2.0.4 fails to link on macOS
# (fmt 12.1 symbols missing from libscorbit_sdk). Move to the next tag once released.
set(SCORBIT_SDK_GIT_TAG "c2f2045abba6ca5f8cbc911a3ca40ee9a3a1b43b" CACHE STRING "scorbit_sdk tag or commit to build when SCORBIT_SDK_ROOT is empty")

# Secrets come from the environment or an untracked .env file (see .env.example);
# neither is committed. SCORBIT_PROVIDER_KEY is compiled in as the default
# providerKey setting; SCORBIT_SDK_ENCRYPT_SECRET is handed to the SDK build.
if(EXISTS "${CMAKE_SOURCE_DIR}/.env")
   file(STRINGS "${CMAKE_SOURCE_DIR}/.env" _env_lines)
   foreach(_line IN LISTS _env_lines)
      if(_line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
         set(_name "${CMAKE_MATCH_1}")
         string(STRIP "${CMAKE_MATCH_2}" _value)
         if(NOT "$ENV{${_name}}" STREQUAL "" OR "${_value}" STREQUAL "")
            continue() # environment wins; empty values are ignored
         endif()
         set(ENV{${_name}} "${_value}")
      endif()
   endforeach()
endif()
if("$ENV{SCORBIT_PROVIDER_KEY}" STREQUAL "")
   message(WARNING "SCORBIT_PROVIDER_KEY is not set: the plugin will have no default provider key")
endif()
if("$ENV{SCORBIT_SDK_ENCRYPT_SECRET}" STREQUAL "" AND NOT SCORBIT_SDK_ROOT)
   message(WARNING "SCORBIT_SDK_ENCRYPT_SECRET is not set: the SDK built here cannot decrypt provider keys")
endif()

include(cmake/CPM.cmake)
include(cmake/vpx_headers.cmake)

# Same pin as scorbit_sdk, added first so CPM reuses it inside the SDK build.
CPMAddPackage(
   NAME nlohmann_json
   URL https://github.com/nlohmann/json/archive/3946872265598aed5a7aea68cad4d9d1f168bd4b.tar.gz
   URL_HASH SHA256=4784ccd3b66d46da6723cb69b3328b8b5548cf82b5c6e917a7636f20a4b40adf
)

if(SCORBIT_SDK_ROOT)
   add_library(scorbit_sdk SHARED IMPORTED)
   file(GLOB _sdk_lib
      "${SCORBIT_SDK_ROOT}/lib/libscorbit_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}"
      "${SCORBIT_SDK_ROOT}/libscorbit_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}"
      "${SCORBIT_SDK_ROOT}/lib/scorbit_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}"
      "${SCORBIT_SDK_ROOT}/scorbit_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
   if(NOT _sdk_lib)
      message(FATAL_ERROR "No scorbit_sdk shared library found under ${SCORBIT_SDK_ROOT}")
   endif()
   list(GET _sdk_lib 0 _sdk_lib)
   set_target_properties(scorbit_sdk PROPERTIES
      IMPORTED_LOCATION "${_sdk_lib}"
      INTERFACE_INCLUDE_DIRECTORIES "${SCORBIT_SDK_ROOT}/include")
   if(WIN32)
      file(GLOB _sdk_implib "${SCORBIT_SDK_ROOT}/lib/scorbit_sdk.lib" "${SCORBIT_SDK_ROOT}/scorbit_sdk.lib")
      if(_sdk_implib)
         list(GET _sdk_implib 0 _sdk_implib)
         set_target_properties(scorbit_sdk PROPERTIES IMPORTED_IMPLIB "${_sdk_implib}")
      endif()
   endif()
else()
   # Needs OpenSSL and libarchive on the host; see README.
   CPMAddPackage(
      NAME scorbit_sdk
      GITHUB_REPOSITORY scorbit-io/scorbit_sdk
      GIT_TAG ${SCORBIT_SDK_GIT_TAG}
      OPTIONS
         "SCORBIT_SDK_SHARED ON"
         "SCORBIT_SDK_BUILD_TESTS OFF"
         "SCORBIT_SDK_BUILD_EXAMPLES OFF"
         "SCORBIT_SDK_ENABLE_NFC_TPM OFF"
   )
endif()

add_library(plugin-scorbit MODULE
   src/common.h
   src/ScorbitPlugin.cpp
   src/Scorbit.h
   src/Scorbit.cpp
   src/DmdTap.h
   src/DmdTap.cpp
   src/qrcodegen.h
   src/qrcodegen.c
)

target_include_directories(plugin-scorbit PRIVATE src "${VPX_HEADERS_DIR}")
target_compile_definitions(plugin-scorbit PRIVATE "SCORBIT_DEFAULT_PROVIDER_KEY=\"$ENV{SCORBIT_PROVIDER_KEY}\"")
target_link_libraries(plugin-scorbit PRIVATE scorbit_sdk nlohmann_json::nlohmann_json)

if(MSVC)
   target_compile_options(plugin-scorbit PRIVATE /W4 /Zc:__cplusplus)
else()
   target_compile_options(plugin-scorbit PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Library names must match plugin.cfg.
set_target_properties(plugin-scorbit PROPERTIES PREFIX "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
   set_target_properties(plugin-scorbit PROPERTIES OUTPUT_NAME "plugin-scorbit64")
else()
   set_target_properties(plugin-scorbit PROPERTIES OUTPUT_NAME "plugin-scorbit")
endif()
if(APPLE)
   set_target_properties(plugin-scorbit PROPERTIES SUFFIX ".dylib" INSTALL_RPATH "@loader_path" BUILD_WITH_INSTALL_RPATH ON)
elseif(UNIX)
   set_target_properties(plugin-scorbit PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
endif()

# Assemble a ready-to-copy plugin folder: <build>/stage/scorbit/
set(STAGE_DIR "${CMAKE_BINARY_DIR}/stage/scorbit")
add_custom_command(TARGET plugin-scorbit POST_BUILD
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/assets"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:plugin-scorbit>" "${STAGE_DIR}/"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/plugin.cfg" "${STAGE_DIR}/"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/scorbit_machines.json" "${STAGE_DIR}/assets/"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:scorbit_sdk>" "${STAGE_DIR}/"
   COMMENT "Staging plugin folder in ${STAGE_DIR}"
)
if(NOT SCORBIT_SDK_ROOT AND (APPLE OR UNIX))
   # The SDK's install name is versioned (libscorbit_sdk.2.dylib / .so.2); stage that name too.
   add_custom_command(TARGET plugin-scorbit POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_SONAME_FILE:scorbit_sdk>" "${STAGE_DIR}/"
   )
endif()
