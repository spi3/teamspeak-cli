cmake_minimum_required(VERSION 3.21)

project(
  teamspeak-cli
  VERSION 0.2.0
  DESCRIPTION "TeamSpeak client plugin CLI"
  LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TS_ENABLE_SANITIZERS "Enable address/undefined sanitizers in Debug builds" ON)
option(TS_ENABLE_TS3_PLUGIN "Build the TeamSpeak 3 client plugin shared library" OFF)
option(TS_ENABLE_REAL_TS3_E2E "Enable the live TeamSpeak client + server end-to-end test" OFF)

set(TS3_PLUGIN_SDK_DIR "$ENV{TS3_PLUGIN_SDK_DIR}" CACHE PATH "Path to the TeamSpeak 3 Client Plugin SDK root")
set(TS3_PLUGIN_SDK_INCLUDE_DIR "" CACHE PATH "Path to the TeamSpeak 3 Client Plugin SDK include directory")
set(TS3_REAL_E2E_CACHE_DIR "$ENV{TS3_REAL_E2E_CACHE_DIR}" CACHE PATH "Cache directory for TeamSpeak client and xdotool assets used by the live E2E test")
set(TS3_REAL_E2E_CLIENT_DIR "$ENV{TS3_REAL_E2E_CLIENT_DIR}" CACHE PATH "Optional path to an extracted TeamSpeak 3 client directory for the live E2E test")
set(TS3_REAL_E2E_CLIENT_VERSION "$ENV{TS3_REAL_E2E_CLIENT_VERSION}" CACHE STRING "Optional TeamSpeak 3 client version override for the live E2E test bootstrap")
set(TS3_REAL_E2E_CLIENT_URL "$ENV{TS3_REAL_E2E_CLIENT_URL}" CACHE STRING "Optional TeamSpeak 3 client download URL override for the live E2E test bootstrap")
set(TS3_REAL_E2E_CLIENT_SHA256 "$ENV{TS3_REAL_E2E_CLIENT_SHA256}" CACHE STRING "Optional TeamSpeak 3 client archive SHA256 override for the live E2E test bootstrap")
set(TS3_REAL_E2E_XDOTOOL "$ENV{TS3_REAL_E2E_XDOTOOL}" CACHE FILEPATH "Optional path to xdotool for the live TeamSpeak E2E test")
set(TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH "$ENV{TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH}" CACHE PATH "Optional LD_LIBRARY_PATH for the xdotool binary used by the live E2E test")

function(ts_enable_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(
      ${target_name}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
    )
  endif()
endfunction()

function(ts_enable_sanitizers target_name)
  if(TS_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined)
    target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

function(ts_apply_defaults target_name)
  set(_ts_disable_sanitizers OFF)
  foreach(_ts_arg IN LISTS ARGN)
    if(_ts_arg STREQUAL "NO_SANITIZERS")
      set(_ts_disable_sanitizers ON)
    endif()
  endforeach()

  target_include_directories(
    ${target_name}
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${TS_GENERATED_INCLUDE_DIR}"
  )
  target_link_libraries(${target_name} PRIVATE Threads::Threads)
  ts_enable_warnings(${target_name})
  if(NOT _ts_disable_sanitizers)
    ts_enable_sanitizers(${target_name})
  endif()
endfunction()

set(TS_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/version.hpp.in "${TS_GENERATED_INCLUDE_DIR}/teamspeak_cli/build/version.hpp" @ONLY)

set(TS_COMMON_SOURCES
  src/teamspeak_cli/bridge/protocol.cpp
  src/teamspeak_cli/cli/completion.cpp
  src/teamspeak_cli/cli/command_router.cpp
  src/teamspeak_cli/config/config_store.cpp
  src/teamspeak_cli/events/event_queue.cpp
  src/teamspeak_cli/output/render.cpp
  src/teamspeak_cli/sdk/backend_factory.cpp
  src/teamspeak_cli/sdk/fake_backend.cpp
  src/teamspeak_cli/sdk/socket_backend.cpp
  src/teamspeak_cli/session/session_service.cpp
  src/teamspeak_cli/util/strings.cpp
)

add_executable(
  ts
  src/teamspeak_cli/main.cpp
  ${TS_COMMON_SOURCES}
)
ts_apply_defaults(ts)

add_executable(
  ts_fake_plugin_host
  tests/fake_plugin_host_main.cpp
  src/teamspeak_cli/bridge/protocol.cpp
  src/teamspeak_cli/bridge/socket_server.cpp
  src/teamspeak_cli/config/config_store.cpp
  src/teamspeak_cli/events/event_queue.cpp
  src/teamspeak_cli/sdk/fake_backend.cpp
  src/teamspeak_cli/util/strings.cpp
)
ts_apply_defaults(ts_fake_plugin_host)

install(TARGETS ts RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES README.md LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/teamspeak-cli")
install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DATADIR}/teamspeak-cli/docs")
install(FILES examples/ts.example.ini DESTINATION "${CMAKE_INSTALL_DATADIR}/teamspeak-cli/examples")

if(BUILD_TESTING)
  add_executable(
    ts_config_store_test
    tests/config_store_test.cpp
    tests/test_support.hpp
    src/teamspeak_cli/config/config_store.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts_config_store_test)
  add_test(NAME ts_config_store_test COMMAND ts_config_store_test)

  add_executable(
    ts_output_render_test
    tests/output_render_test.cpp
    tests/test_support.hpp
    src/teamspeak_cli/output/render.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts_output_render_test)
  add_test(NAME ts_output_render_test COMMAND ts_output_render_test)

  add_executable(
    ts_session_service_test
    tests/session_service_test.cpp
    tests/test_support.hpp
    src/teamspeak_cli/events/event_queue.cpp
    src/teamspeak_cli/sdk/fake_backend.cpp
    src/teamspeak_cli/session/session_service.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts_session_service_test)
  add_test(NAME ts_session_service_test COMMAND ts_session_service_test)

  add_executable(
    ts_event_translation_test
    tests/event_translation_test.cpp
    tests/test_support.hpp
    src/teamspeak_cli/events/event_queue.cpp
    src/teamspeak_cli/sdk/fake_backend.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts_event_translation_test)
  add_test(NAME ts_event_translation_test COMMAND ts_event_translation_test)

  add_executable(
    ts_cli_router_test
    tests/cli_router_test.cpp
    tests/test_support.hpp
    ${TS_COMMON_SOURCES}
  )
  ts_apply_defaults(ts_cli_router_test)
  add_test(NAME ts_cli_router_test COMMAND ts_cli_router_test)

  add_executable(
    ts_socket_backend_test
    tests/socket_backend_test.cpp
    tests/test_support.hpp
    src/teamspeak_cli/bridge/protocol.cpp
    src/teamspeak_cli/bridge/socket_server.cpp
    src/teamspeak_cli/events/event_queue.cpp
    src/teamspeak_cli/sdk/fake_backend.cpp
    src/teamspeak_cli/sdk/socket_backend.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts_socket_backend_test)
  add_test(NAME ts_socket_backend_test COMMAND ts_socket_backend_test)

  add_test(NAME ts_fake_cli_smoke_test COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_fake_cli_smoke.sh" "$<TARGET_FILE:ts>")
  add_test(
    NAME ts_plugin_bridge_e2e_test
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_plugin_bridge_e2e.sh"
      "$<TARGET_FILE:ts>"
      "$<TARGET_FILE:ts_fake_plugin_host>"
  )
  set_tests_properties(ts_plugin_bridge_e2e_test PROPERTIES TIMEOUT 60)

  add_test(
    NAME ts_e2e_shell_syntax_test
    COMMAND
      bash -n
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/bootstrap_real_runtime_deps.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/real_plugin_runtime_common.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_real_plugin_server_env.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_real_plugin_server_e2e.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/stop_real_plugin_server_env.sh"
  )
endif()

if(TS_ENABLE_TS3_PLUGIN)
  set(_ts3_hints "")
  if(TS3_PLUGIN_SDK_DIR)
    list(APPEND _ts3_hints
      "${TS3_PLUGIN_SDK_DIR}/include"
      "${TS3_PLUGIN_SDK_DIR}"
    )
  endif()

  if(NOT TS3_PLUGIN_SDK_INCLUDE_DIR)
    if(EXISTS "${TS3_PLUGIN_SDK_DIR}/include/plugin_definitions.h")
      set(TS3_PLUGIN_SDK_INCLUDE_DIR "${TS3_PLUGIN_SDK_DIR}/include")
    else()
      find_path(
        TS3_PLUGIN_SDK_INCLUDE_DIR
        NAMES plugin_definitions.h
        HINTS ${_ts3_hints}
        PATH_SUFFIXES include
      )
    endif()
  endif()

  if(NOT TS3_PLUGIN_SDK_INCLUDE_DIR)
    message(
      FATAL_ERROR
      "TS_ENABLE_TS3_PLUGIN=ON but the TeamSpeak 3 Client Plugin SDK headers were not found. "
      "Set -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk or -DTS3_PLUGIN_SDK_INCLUDE_DIR=/path/to/include."
    )
  endif()

  add_library(
    ts3cli_plugin
    SHARED
    src/teamspeak_cli/bridge/protocol.cpp
    src/teamspeak_cli/bridge/socket_server.cpp
    src/teamspeak_cli/events/event_queue.cpp
    src/teamspeak_cli/plugin/plugin_exports.cpp
    src/teamspeak_cli/sdk/plugin_host_backend.cpp
    src/teamspeak_cli/util/strings.cpp
  )
  ts_apply_defaults(ts3cli_plugin NO_SANITIZERS)
  target_include_directories(ts3cli_plugin PRIVATE "${TS3_PLUGIN_SDK_INCLUDE_DIR}")
  set_target_properties(ts3cli_plugin PROPERTIES PREFIX "")
endif()

if(BUILD_TESTING AND TS_ENABLE_REAL_TS3_E2E)
  if(NOT TS_ENABLE_TS3_PLUGIN)
    message(FATAL_ERROR "TS_ENABLE_REAL_TS3_E2E=ON requires TS_ENABLE_TS3_PLUGIN=ON.")
  endif()

  add_test(
    NAME ts_real_plugin_server_e2e_test
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_real_plugin_server_e2e.sh"
      "$<TARGET_FILE:ts>"
      "$<TARGET_FILE:ts3cli_plugin>"
  )
  set_tests_properties(ts_real_plugin_server_e2e_test PROPERTIES TIMEOUT 300)

  set(_ts_real_e2e_env "")
  if(TS3_REAL_E2E_CACHE_DIR)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_CACHE_DIR=${TS3_REAL_E2E_CACHE_DIR}")
  endif()
  if(TS3_REAL_E2E_CLIENT_DIR)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_CLIENT_DIR=${TS3_REAL_E2E_CLIENT_DIR}")
  endif()
  if(TS3_REAL_E2E_CLIENT_VERSION)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_CLIENT_VERSION=${TS3_REAL_E2E_CLIENT_VERSION}")
  endif()
  if(TS3_REAL_E2E_CLIENT_URL)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_CLIENT_URL=${TS3_REAL_E2E_CLIENT_URL}")
  endif()
  if(TS3_REAL_E2E_CLIENT_SHA256)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_CLIENT_SHA256=${TS3_REAL_E2E_CLIENT_SHA256}")
  endif()
  if(TS3_REAL_E2E_XDOTOOL)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_XDOTOOL=${TS3_REAL_E2E_XDOTOOL}")
  endif()
  if(TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH)
    list(APPEND _ts_real_e2e_env "TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH=${TS3_REAL_E2E_XDOTOOL_LIBRARY_PATH}")
  endif()
  if(_ts_real_e2e_env)
    set_tests_properties(ts_real_plugin_server_e2e_test PROPERTIES ENVIRONMENT "${_ts_real_e2e_env}")
  endif()
endif()
