# The transport tests need no Visual Pinball headers and no Scorbit SDK: they
# link scorbit-transport and drive it against the scripted peer in TestPeer.h.

add_executable(wire_codec_test WireCodecTest.cpp Check.h)
target_link_libraries(wire_codec_test PRIVATE scorbit-transport)
target_compile_options(wire_codec_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME wire_codec COMMAND wire_codec_test)

# No sockets, so this one runs on every platform, Windows included.
add_executable(memory_bridge_test MemoryBridgeTest.cpp Check.h)
target_link_libraries(memory_bridge_test PRIVATE scorbit-transport)
target_compile_options(memory_bridge_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME memory_bridge COMMAND memory_bridge_test)
set_tests_properties(memory_bridge PROPERTIES TIMEOUT 60)

# Header-only and pure, so it runs everywhere too. Links the transport for its include path.
add_executable(overlay_placement_test OverlayPlacementTest.cpp Check.h)
target_link_libraries(overlay_placement_test PRIVATE scorbit-transport)
target_compile_options(overlay_placement_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME overlay_placement COMMAND overlay_placement_test)

# Pure as well: the overlay's firmware rules and shade table, and the status notifier.
add_executable(overlay_state_test OverlayStateTest.cpp Check.h)
target_link_libraries(overlay_state_test PRIVATE scorbit-transport)
target_compile_options(overlay_state_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME overlay_state COMMAND overlay_state_test)

add_executable(status_notice_test StatusNoticeTest.cpp Check.h)
target_link_libraries(status_notice_test PRIVATE scorbit-transport)
target_compile_options(status_notice_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME status_notice COMMAND status_notice_test)

add_executable(status_light_test StatusLightTest.cpp Check.h)
target_link_libraries(status_light_test PRIVATE scorbit-transport)
target_compile_options(status_light_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME status_light COMMAND status_light_test)

add_executable(status_light_raster_test StatusLightRasterTest.cpp Check.h)
target_link_libraries(status_light_raster_test PRIVATE scorbit-transport)
target_compile_options(status_light_raster_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME status_light_raster COMMAND status_light_raster_test)

add_executable(override_chain_test OverrideChainTest.cpp Check.h)
target_link_libraries(override_chain_test PRIVATE scorbit-transport)
target_compile_options(override_chain_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME override_chain COMMAND override_chain_test)

# Threads but no sockets: the seqlock, the latch and the overlay cache.
add_executable(handoffs_test HandoffsTest.cpp Check.h)
target_link_libraries(handoffs_test PRIVATE scorbit-transport)
target_compile_options(handoffs_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME handoffs COMMAND handoffs_test)
set_tests_properties(handoffs PROPERTIES TIMEOUT 60)

# Pure, plus a temp file: Scorbit.Declare's checks, native paths, WireDump redaction (SB-5990).
add_executable(authored_table_test AuthoredTableTest.cpp Check.h)
target_link_libraries(authored_table_test PRIVATE scorbit-transport)
target_compile_options(authored_table_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME authored_table COMMAND authored_table_test)

# Pure, plus temp files: the script's read-back store and the player picture copies (protocol 1.2).
add_executable(read_back_test ReadBackTest.cpp Check.h)
target_link_libraries(read_back_test PRIVATE scorbit-transport)
target_compile_options(read_back_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME read_back COMMAND read_back_test)

# Temp folders: the add-only install of the bundled table-script binding.
add_executable(binding_install_test BindingInstallTest.cpp Check.h)
target_link_libraries(binding_install_test PRIVATE scorbit-transport)
target_compile_options(binding_install_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME binding_install COMMAND binding_install_test)

if(NOT WIN32)
   # The peer speaks POSIX sockets directly. The worker's Windows path is
   # written but nothing in CI compiles it yet, so this test does not claim to
   # cover it.
   add_executable(socket_worker_test SocketWorkerTest.cpp TestPeer.h Check.h)
   target_link_libraries(socket_worker_test PRIVATE scorbit-transport)
   target_compile_options(socket_worker_test PRIVATE ${SCORBIT_WARNINGS})
   add_test(NAME socket_worker COMMAND socket_worker_test)
   set_tests_properties(socket_worker PROPERTIES TIMEOUT 60)
endif()
