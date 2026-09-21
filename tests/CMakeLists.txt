add_library(ff-core STATIC
  ${CMAKE_SOURCE_DIR}/src/ff-analysis.c
  ${CMAKE_SOURCE_DIR}/src/ff-handoff.c
  ${CMAKE_SOURCE_DIR}/src/ff-licence.c
  ${CMAKE_SOURCE_DIR}/vendor/kissfft/kiss_fft.c
  ${CMAKE_SOURCE_DIR}/vendor/kissfft/kiss_fftr.c
  ${CMAKE_SOURCE_DIR}/vendor/monocypher/monocypher.c
  ${CMAKE_SOURCE_DIR}/vendor/monocypher/monocypher-ed25519.c)
# Vendored third-party sources trip -Wsign-compare under this project's -Werror;
# scoped to the vendor files only, never to our own sources.
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/vendor/kissfft/kiss_fft.c ${CMAKE_SOURCE_DIR}/vendor/kissfft/kiss_fftr.c
  ${CMAKE_SOURCE_DIR}/vendor/monocypher/monocypher.c ${CMAKE_SOURCE_DIR}/vendor/monocypher/monocypher-ed25519.c
  PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare"
)
target_include_directories(ff-core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/vendor/kissfft ${CMAKE_SOURCE_DIR}/vendor/monocypher)
target_compile_definitions(ff-core PUBLIC kiss_fft_scalar=float)
target_link_libraries(ff-core PUBLIC m)
foreach(t analysis handoff licence)
  add_executable(test_${t} test_${t}.c)
  target_link_libraries(test_${t} PRIVATE ff-core)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
target_link_libraries(test_handoff PRIVATE pthread)

# ff-pack.c cannot join ff-core: it needs libobs (obs_data, the os_* file helpers) and
# plugin-support's obs_log. Link those in for this one test rather than leaving the symlink
# guard with no gate of its own -- see the header comment in test_pack.c.
add_executable(test_pack test_pack.c ${CMAKE_SOURCE_DIR}/src/ff-pack.c)
target_link_libraries(test_pack PRIVATE ff-core plugin-support OBS::libobs)
target_include_directories(test_pack PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pack COMMAND test_pack)
set_tests_properties(pack PROPERTIES SKIP_RETURN_CODE 77)

add_executable(verify_cli verify_cli.c)
target_link_libraries(verify_cli PRIVATE ff-core)

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
