# The transport tests need no Visual Pinball headers and no Scorbit SDK: they
# link scorbit-transport and drive it against the scripted peer in TestPeer.h.

add_executable(wire_codec_test WireCodecTest.cpp Check.h)
target_link_libraries(wire_codec_test PRIVATE scorbit-transport)
target_compile_options(wire_codec_test PRIVATE ${SCORBIT_WARNINGS})
add_test(NAME wire_codec COMMAND wire_codec_test)

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
