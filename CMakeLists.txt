cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# plugin-support.c.in (added by cmake/common/helpers_common.cmake, before find_package(libobs)
# above ran) now stamps LOG_WARNING/LOG_ERROR severity into every obs_log() line -- give it the
# libobs include dirs it needs for util/base.h's LOG_* constants.
if(TARGET plugin-support)
  target_link_libraries(plugin-support PUBLIC OBS::libobs)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# --- ff-core: everything every Foxfire plugin needs -------------------------------------------
#
# Foxfire ships as SEVERAL plugins so nobody has to install all of it to use one part. They share
# a codebase and this is where the shared part lives: pack loading, licence verification, the
# audio analysis and the layer renderer.
#
# STATIC, not SHARED, and that matters. ff-pack.c calls obs_module_config_path(), which expands to
# obs_current_module() -- a symbol OBS_DECLARE_MODULE() defines separately inside each plugin. A
# static library is linked into each plugin and resolves that symbol per plugin, so every one of
# them gets its own module identity. A shared library would have to pick one.
#
# Licence verification belongs here and NOWHERE ELSE. It has to produce bytes identical to what
# packforge signs, and tools/licence-interop.py exists to catch that drifting; a second copy would
# double the surface that gate has to cover.
add_library(
  ff-core
  STATIC
  src/ff-analysis.c
  src/ff-viseme.c
  src/ff-handoff.c
  src/ff-licence.c
  src/ff-audio.c
  src/ff-pack.c
  src/ff-layers.c
  vendor/kissfft/kiss_fft.c
  vendor/kissfft/kiss_fftr.c
  vendor/monocypher/monocypher.c
  vendor/monocypher/monocypher-ed25519.c
)
target_include_directories(ff-core PUBLIC src vendor/kissfft vendor/monocypher)
target_compile_definitions(ff-core PUBLIC kiss_fft_scalar=float)
# MSVC ships <stdatomic.h> behind an opt-in and #errors "C atomic support is not enabled"
# without it, whatever /std: says -- and ff-handoff.h, the seqlock the audio thread hands
# frames across, is built on atomic_uint. PUBLIC because that header is public: every target
# linking ff-core compiles it too. CMAKE_C_STANDARD is already 17, which is the other half
# of the requirement. (Microsoft still defines __STDC_NO_ATOMICS__ even with the flag on, so
# do not reach for that macro to detect this.)
target_compile_options(ff-core PUBLIC $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
# m: the analysis is all sinf/powf/log10f. There is no such library on MSVC -- the math
# functions live in the CRT -- and asking for one that does not exist fails the link.
target_link_libraries(ff-core PUBLIC OBS::libobs $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>)
if(TARGET plugin-support)
  target_link_libraries(ff-core PUBLIC plugin-support)
endif()
set_property(TARGET ff-core PROPERTY POSITION_INDEPENDENT_CODE ON)
# Vendored third-party sources trip signed/unsigned comparison warnings under this project's
# warnings-as-error; scoped to the vendor files only, never to our own sources. cl rejects a
# GCC flag outright rather than ignoring it ("command line error D8021: invalid numeric
# argument '/Wno-sign-compare'"), so each compiler gets its own spelling. C4018 is MSVC's
# -Wsign-compare and fires at /W3, which is what this build uses; its relatives C4245 and
# C4389 are /W4-only and are deliberately not listed.
set_source_files_properties(
  vendor/kissfft/kiss_fft.c
  vendor/kissfft/kiss_fftr.c
  vendor/monocypher/monocypher.c
  vendor/monocypher/monocypher-ed25519.c
  PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/wd4018,-Wno-sign-compare>"
)

# --- obs-foxfire: the visualizer source and the effects filter ---------------------------------
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/ff-props.c src/ff-source.c src/ff-filter.c)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ff-core)

# curl carries the EventSub connection. obs-deps ships it on every platform OBS builds for, and
# curl's own WebSocket API is experimental and absent from the libcurl here (measured), so
# CONNECT_ONLY hands over the TLS socket and src/ff-ws*.c do the framing. Only the alerts plugin
# needs it -- the visualizer talks to nothing.
find_package(CURL REQUIRED)

# --- obs-foxfire-alerts: the alerts engine, installable on its own -----------------------------
# Its own .so, its own entry in OBS's plugin list, its own enable/disable. A streamer who wants
# alerts installs this one and nothing else; it still reads the same packs directory (see
# ff_shared_config_path) so packs are not split across plugins.
add_library(obs-foxfire-alerts MODULE)
target_sources(
  obs-foxfire-alerts
  PRIVATE
    src/alerts-main.c
    src/ff-alert.c
    src/ff-alert-source.c
    src/ff-alert-queue.c
    src/ff-ws.c
    src/ff-ws-conn.c
    src/ff-net.c
    src/ff-eventsub.c
    src/ff-twitch-api.c
    src/ff-twitch.c
    src/ff-session.c
)
target_link_libraries(obs-foxfire-alerts PRIVATE ff-core CURL::libcurl)
set_target_properties_plugin(obs-foxfire-alerts PROPERTIES OUTPUT_NAME obs-foxfire-alerts)
option(FOXFIRE_BUILD_TESTS "Build ff unit tests" OFF)
if(FOXFIRE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
