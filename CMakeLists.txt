cmake_minimum_required(VERSION 3.20)

project(
  AAFR
  VERSION 0.1.0
  DESCRIPTION "Research prototype for an intelligent browser with adaptive forwarding"
  LANGUAGES CXX
)

option(AAFR_BUILD_TESTS "Build smoke and unit tests" ON)
option(AAFR_BUILD_QT_BROWSER "Build the Qt WebEngine browser shell when Qt is installed" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(runtime_core STATIC
  runtime/decision/weighted_decision_model.cpp
  runtime/transition/strategy_transition_manager.cpp
  runtime/telemetry/jsonl_telemetry_sink.cpp
)

target_include_directories(runtime_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(runtime_core PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(runtime_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(runtime_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(browser_demo
  browser/qt_browser/browser_demo.cpp
)

target_link_libraries(browser_demo PRIVATE runtime_core)

if(AAFR_BUILD_TESTS)
  enable_testing()

  add_executable(runtime_core_tests
    tests/unit/runtime_core_smoke_test.cpp
  )

  target_link_libraries(runtime_core_tests PRIVATE runtime_core)

  add_test(NAME runtime_core_smoke COMMAND runtime_core_tests)
endif()

if(AAFR_BUILD_QT_BROWSER)
  message(FATAL_ERROR
    "Qt WebEngine browser shell is intentionally not wired yet. "
    "Build the research runtime first, then add Qt 6 WebEngine after toolchain setup."
  )
endif()
