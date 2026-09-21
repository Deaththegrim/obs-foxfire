# The tests link the PRODUCTION ff-core, not a copy of its source list.
#
# There used to be a second ff-core defined here with its own enumeration of the shared sources.
# Two lists of the same thing drift: this one had no ff-pack.c or ff-layers.c, so test_pack had to
# compile ff-pack.c a second time with different flags, and anything added to the plugin's core
# was invisible to the tests until somebody noticed. One definition, in the top-level
# CMakeLists.txt, linked here.
foreach(t analysis handoff licence pack viseme)
  add_executable(test_${t} test_${t}.c)
  target_link_libraries(test_${t} PRIVATE ff-core)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# ff-alert.c belongs to the ALERTS plugin, not to the shared core -- nothing else needs it, and
# putting it in core would link a text-source lookup into the visualizer for no reason. So this
# one test compiles it directly, the same way test_pack used to compile ff-pack.c.
add_executable(test_alert test_alert.c ${CMAKE_SOURCE_DIR}/src/ff-alert.c ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c)

add_executable(test_ws test_ws.c ${CMAKE_SOURCE_DIR}/src/ff-ws.c)
target_link_libraries(test_ws PRIVATE ff-core)
add_test(NAME ws COMMAND test_ws)

# The connection layer takes its socket as two function pointers, so this links no transport at
# all -- the whole read loop is driven from a scripted buffer.
add_executable(test_ws_conn test_ws_conn.c ${CMAKE_SOURCE_DIR}/src/ff-ws.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c)
target_link_libraries(test_ws_conn PRIVATE ff-core)
add_test(NAME ws_conn COMMAND test_ws_conn)
target_link_libraries(test_alert PRIVATE ff-core)
add_test(NAME alert COMMAND test_alert)
target_link_libraries(test_handoff PRIVATE pthread)
# test_pack builds and tears down real directories and symlinks; on Windows creating a link needs
# a privileged account, so it reports 77 (skip) there rather than a pass that inspected nothing.
set_tests_properties(pack PROPERTIES SKIP_RETURN_CODE 77)

add_executable(verify_cli verify_cli.c)
target_link_libraries(verify_cli PRIVATE ff-core)

# Not a test: there is no corpus in the repo to run it against, and a test that skips when its
# input is missing is a test that passes having inspected nothing. It is built so that pointing
# it at a voice is one command rather than a scratch program written again each time.
add_executable(calibrate_cli calibrate_cli.c)
target_link_libraries(calibrate_cli PRIVATE ff-core)

# Python signs, C verifies. The unit tests above sign and verify with the same
# Monocypher code, so only this can catch a divergence from python-cryptography,
# which is what packforge mints real licences with. Skips (77) when the
# cryptography module is absent rather than failing the build.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME licence_interop
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/licence-interop.py $<TARGET_FILE:verify_cli>)
  set_tests_properties(licence_interop PROPERTIES SKIP_RETURN_CODE 77)
endif()

# A real socket, a real curl connection, and a WebSocket server built out of raw sockets in
# Python. The unit test proves the state machine; this proves it is wired to a socket. Skips (77)
# when Python is absent rather than passing having inspected nothing.
add_executable(test_eventsub test_eventsub.c ${CMAKE_SOURCE_DIR}/src/ff-eventsub.c
  ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c)
target_link_libraries(test_eventsub PRIVATE ff-core)
add_test(NAME eventsub COMMAND test_eventsub)

add_executable(test_url test_url.c ${CMAKE_SOURCE_DIR}/src/ff-ws.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(test_url PRIVATE ff-core CURL::libcurl)
add_test(NAME url COMMAND test_url)

# ff-twitch.c is a thread and a socket; these two functions are neither, which is why they were
# pulled out of it. Linked directly rather than through the plugin: the test needs no OBS.
add_executable(test_token test_token.c ${CMAKE_SOURCE_DIR}/src/ff-twitch-api.c
  ${CMAKE_SOURCE_DIR}/src/ff-eventsub.c ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws.c ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c
  ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(test_token PRIVATE ff-core CURL::libcurl)
add_test(NAME token COMMAND test_token)

add_executable(test_twitch_policy test_twitch_policy.c ${CMAKE_SOURCE_DIR}/src/ff-twitch.c
  ${CMAKE_SOURCE_DIR}/src/ff-session.c ${CMAKE_SOURCE_DIR}/src/ff-twitch-api.c ${CMAKE_SOURCE_DIR}/src/ff-eventsub.c
  ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c ${CMAKE_SOURCE_DIR}/src/ff-ws.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(test_twitch_policy PRIVATE ff-core CURL::libcurl)
add_test(NAME twitch_policy COMMAND test_twitch_policy)

# The session state machine: scripted messages and an invented clock, no socket and no OBS.
add_executable(test_session test_session.c ${CMAKE_SOURCE_DIR}/src/ff-session.c
  ${CMAKE_SOURCE_DIR}/src/ff-twitch.c ${CMAKE_SOURCE_DIR}/src/ff-twitch-api.c
  ${CMAKE_SOURCE_DIR}/src/ff-eventsub.c ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws.c ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c
  ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(test_session PRIVATE ff-core CURL::libcurl)
add_test(NAME session COMMAND test_session)

add_executable(twitch_cli twitch_cli.c ${CMAKE_SOURCE_DIR}/src/ff-twitch-api.c
  ${CMAKE_SOURCE_DIR}/src/ff-eventsub.c ${CMAKE_SOURCE_DIR}/src/ff-alert-queue.c
  ${CMAKE_SOURCE_DIR}/src/ff-ws.c ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c
  ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(twitch_cli PRIVATE ff-core CURL::libcurl)
if(Python3_Interpreter_FOUND)
  add_test(NAME twitch_api
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/twitch-api-proof.py $<TARGET_FILE:twitch_cli>)
  set_tests_properties(twitch_api PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(ws_cli ws_cli.c ${CMAKE_SOURCE_DIR}/src/ff-ws.c ${CMAKE_SOURCE_DIR}/src/ff-ws-conn.c
  ${CMAKE_SOURCE_DIR}/src/ff-net.c)
target_link_libraries(ws_cli PRIVATE ff-core CURL::libcurl)
if(Python3_Interpreter_FOUND)
  add_test(NAME ws_server
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ws-server-proof.py $<TARGET_FILE:ws_cli>)
  set_tests_properties(ws_server PROPERTIES SKIP_RETURN_CODE 77)
endif()
