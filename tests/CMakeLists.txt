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
