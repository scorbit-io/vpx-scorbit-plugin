cmake_minimum_required(VERSION 3.28)

project(vpx-scorbit-plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The plugin holds no Scorbit identity and no secrets: scorbitd is the Scorbitron,
# and the plugin only talks to it over a local socket (SB-5991).
#
# The transport carries no Visual Pinball dependency, so it builds and tests on its
# own, with no network. CI uses that for its compile gate on every platform; the full
# plugin also needs the VPX headers, which are fetched at configure time.
option(SCORBIT_TRANSPORT_ONLY "Build only the transport library and its tests" OFF)

if(NOT SCORBIT_TRANSPORT_ONLY)
   include(cmake/vpx_headers.cmake)
endif()

if(MSVC)
   # Mirror the POSIX set: C4100 is -Wunused-parameter, which POSIX turns off; C4201
   # (nameless struct/union) needs -pedantic and C4459 (hides a global) -Wshadow,
   # neither of which POSIX enables. All three fire in VPX headers too.
   set(SCORBIT_WARNINGS /W4 /Zc:__cplusplus /wd4100 /wd4201 /wd4459)
else()
   set(SCORBIT_WARNINGS -Wall -Wextra -Wno-unused-parameter)
endif()

# The wire protocol and the socket worker carry no Visual Pinball dependency, so they
# build and test on their own against a scripted peer.
add_library(scorbit-transport STATIC
   src/WireProtocol.h
   src/WireProtocol.cpp
   src/MemoryBridge.h
   src/MemoryBridge.cpp
   src/OverlayPlacement.h
   src/OverlayState.h
   src/OverlayCache.h
   src/OverrideChain.h
   src/SeqLock.h
   src/Latch.h
   src/StatusNotice.h
   src/StatusNotice.cpp
   src/SocketWorker.h
   src/SocketWorker.cpp
   src/WireDump.h
   src/WireDump.cpp
)
target_include_directories(scorbit-transport PUBLIC src)
target_compile_options(scorbit-transport PRIVATE ${SCORBIT_WARNINGS})
if(MSVC)
   # getenv and fopen in WireDump draw C4996, MSVC's deprecation nag for the CRT.
   # Both calls are checked before use, and the _s replacements it suggests are
   # Microsoft-only, so taking them would put platform branching into a file that
   # has none. scorbitd sets the same definition for every MSVC target.
   target_compile_definitions(scorbit-transport PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
find_package(Threads REQUIRED)
target_link_libraries(scorbit-transport PUBLIC Threads::Threads)
if(WIN32)
   target_link_libraries(scorbit-transport PUBLIC ws2_32)
endif()

if(NOT SCORBIT_TRANSPORT_ONLY)

add_library(plugin-scorbit MODULE
   src/common.h
   src/ScorbitPlugin.cpp
   src/DmdTap.h
   src/DmdTap.cpp
   src/DmdOverlay.h
   src/DmdOverlay.cpp
   src/OverlayPlacement.h
   src/VpxSessionSource.h
   src/VpxSessionSource.cpp
   src/ScorbitPluginAPI.h
   src/QuitHook.h
)
if(APPLE)
   enable_language(OBJCXX)
   target_sources(plugin-scorbit PRIVATE src/QuitHook_mac.mm)
   set_source_files_properties(src/QuitHook_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
   target_link_libraries(plugin-scorbit PRIVATE "-framework AppKit")
endif()

target_include_directories(plugin-scorbit PRIVATE src "${VPX_HEADERS_DIR}")
target_compile_definitions(plugin-scorbit PRIVATE
   "SCORBIT_PLUGIN_VERSION=\"${PROJECT_VERSION}\""
   "SCORBIT_VPX_PLUGIN_API_COMMIT=\"${VPX_PLUGIN_API_COMMIT}\""
   "SCORBIT_VPX_PINNED_VERSION=\"${VPX_PINNED_VERSION}\""
   "SCORBIT_VPX_PINNED_REVISION=\"${VPX_PINNED_REVISION}\""
)
target_link_libraries(plugin-scorbit PRIVATE scorbit-transport)
if(MSVC)
   # Same reason as scorbit-transport: fopen in DmdTap is checked before use.
   target_compile_definitions(plugin-scorbit PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_options(plugin-scorbit PRIVATE ${SCORBIT_WARNINGS})

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
# A custom target is out of date on every build in every generator, so a plugin.cfg-only
# edit is staged without a relink (LINK_DEPENDS would cover Make and Ninja only, not Visual
# Studio). The folder is recreated each time, so files a previous revision staged (the SDK
# library and assets/ until SB-5991) never survive in what is copied as the plugin.
add_custom_target(stage-plugin ALL
   COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STAGE_DIR}"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}"
   COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:plugin-scorbit>" "${STAGE_DIR}/"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/plugin.cfg" "${STAGE_DIR}/"
   COMMENT "Staging plugin folder in ${STAGE_DIR}"
   VERBATIM
)
add_dependencies(stage-plugin plugin-scorbit)

endif()  # NOT SCORBIT_TRANSPORT_ONLY

option(SCORBIT_BUILD_TESTS "Build the transport tests" ON)
if(SCORBIT_BUILD_TESTS)
   enable_testing()
   add_subdirectory(test)
endif()
