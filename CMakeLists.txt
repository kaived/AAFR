
cmake_minimum_required(VERSION 3.22)

project(AAFR VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)
if(WIN32)
    # Private QWindowsWindow interface: folds the native caption into the
    # client area so the tab strip can act as the title bar.
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()

qt_standard_project_setup()
qt_add_resources(AAFR_BROWSER_RESOURCES browser/qt_webengine/browser_assets.qrc)

# Plain C++17 targets below disable Qt's automatic code generators.
function(aafr_plain_cxx target)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endfunction()

# Runtime core: plain C++17, no Qt dependency.
add_library(aafr-runtime STATIC
    runtime/common/types.h
    runtime/interfaces/interfaces.h
    runtime/execution/action_space.h
    runtime/decision/weighted_decision_model.h
    runtime/decision/weighted_decision_model.cpp
    runtime/transition/strategy_transition_manager.h
    runtime/transition/strategy_transition_manager.cpp
    runtime/telemetry/jsonl_telemetry_sink.h
    runtime/telemetry/jsonl_telemetry_sink.cpp
)
aafr_plain_cxx(aafr-runtime)
target_include_directories(aafr-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# V0.3 telemetry core and runtime observability contracts: plain C++17, so the
# future runtime can use them without linking Qt or the browser UI.
add_library(aafr-telemetry STATIC
    network/telemetry/telemetry_event.h
    network/telemetry/telemetry_event.cpp
    network/telemetry/bounded_queue.h
    network/telemetry/jsonl_telemetry_writer.h
    network/telemetry/jsonl_telemetry_writer.cpp
    network/telemetry/telemetry_pipeline.h
    network/telemetry/telemetry_pipeline.cpp
    network/tshark/tshark_fields.h
    network/tshark/tshark_fields.cpp
    runtime/observability/runtime_contracts.h
    runtime/observability/runtime_contracts.cpp
)
aafr_plain_cxx(aafr-telemetry)
target_include_directories(aafr-telemetry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aafr-telemetry PUBLIC Threads::Threads)

# V0.4 NetworkState, feature engineering, constraints, the algorithm capability
# registry and eligibility evaluation: plain C++17, no Qt. Holds no algorithm
# implementation and performs no selection or switching.
add_library(aafr-network-state STATIC
    network/network_state/execution_environment.h
    network/network_state/network_state.h
    network/network_state/network_state.cpp
    network/network_state/feature_normalizer.h
    network/network_state/feature_normalizer.cpp
    network/network_state/feature_validator.h
    network/network_state/feature_validator.cpp
    network/network_state/feature_extractor.h
    network/network_state/feature_extractor.cpp
    network/network_state/measurement_quality.h
    network/network_state/measurement_quality.cpp
    network/network_state/network_state_builder.h
    network/network_state/network_state_builder.cpp
    network/network_state/network_constraints.h
    network/network_state/network_constraints.cpp
    network/network_state/network_state_service.h
    network/network_state/network_state_service.cpp
    algorithms/algorithm_id.h
    algorithms/algorithm_capability.h
    algorithms/capability_registry.h
    algorithms/capability_registry.cpp
    algorithms/eligibility_reason.h
    algorithms/eligibility_reason.cpp
    algorithms/eligibility_evaluator.h
    algorithms/eligibility_evaluator.cpp
)
aafr_plain_cxx(aafr-network-state)
target_link_libraries(aafr-network-state PUBLIC aafr-telemetry)

# Network monitors (Qt Core only, no widgets).
add_library(aafr-network-monitor STATIC
    network/monitor/network_monitor.h
    network/monitor/network_monitor.cpp
    network/tshark/tshark_monitor.h
    network/tshark/tshark_monitor.cpp
)
find_package(Qt6 REQUIRED COMPONENTS Network)
# Qt Network: QNetworkInterface finds the connected adapter for capture.
target_link_libraries(aafr-network-monitor PUBLIC aafr-telemetry Qt6::Core Qt6::Network)

# V0.5 controlled forwarding engine: the forwarding graph and problem, the
# ten path algorithms, the simulated and loopback forwarding executors, the
# manual algorithm execution engine and the benchmark runner. Plain C++17,
# no Qt. Performs no automatic algorithm selection or switching.
add_library(aafr-forwarding STATIC
    routing/topology/forwarding_graph.h
    routing/topology/forwarding_graph.cpp
    routing/topology/forwarding_problem.h
    routing/topology/forwarding_problem.cpp
    routing/topology/scenario_catalog.h
    routing/topology/scenario_catalog.cpp
    routing/forwarding/forwarding_executor.h
    routing/forwarding/forwarding_executor.cpp
    routing/forwarding/loopback_forwarding.h
    routing/forwarding/loopback_forwarding.cpp
    algorithms/common/path_algorithm.h
    algorithms/common/path_algorithm.cpp
    algorithms/common/algorithm_makers.h
    algorithms/common/algorithm_factory.cpp
    algorithms/common/metaheuristic_support.h
    algorithms/common/metaheuristic_support.cpp
    algorithms/dijkstra/dijkstra.cpp
    algorithms/astar/astar.cpp
    algorithms/bellman_ford/bellman_ford.cpp
    algorithms/aco/aco.cpp
    algorithms/pso/pso.cpp
    algorithms/ga/ga.cpp
    algorithms/abc/abc.cpp
    algorithms/gwo/gwo.cpp
    algorithms/firefly/firefly.cpp
    algorithms/woa/woa.cpp
    algorithms/execution/implementation_verifier.h
    algorithms/execution/implementation_verifier.cpp
    algorithms/execution/algorithm_execution_engine.h
    algorithms/execution/algorithm_execution_engine.cpp
    experiments/benchmark/benchmark_runner.h
    experiments/benchmark/benchmark_runner.cpp
    routing/controller/forwarding_service.h
    routing/controller/forwarding_service.cpp
)
aafr_plain_cxx(aafr-forwarding)
target_link_libraries(aafr-forwarding PUBLIC aafr-network-state Threads::Threads)
if(WIN32)
    target_link_libraries(aafr-forwarding PRIVATE ws2_32)
endif()

# Reproducible benchmark over the scenario catalog; writes JSONL and CSV.
add_executable(aafr-benchmark tools/benchmark/benchmark_main.cpp)
aafr_plain_cxx(aafr-benchmark)
target_link_libraries(aafr-benchmark PRIVATE aafr-forwarding)

# Browser UI shared by the application and the Qt tests.
add_library(aafr-browser-ui STATIC
    browser/qt_webengine/browser_window.h
    browser/qt_webengine/browser_window.cpp
    browser/qt_webengine/tab_manager.h
    browser/qt_webengine/tab_manager.cpp
    browser/qt_webengine/browser_theme.h
    browser/qt_webengine/browser_theme.cpp
    browser/qt_webengine/browser_event_collector.h
    browser/qt_webengine/browser_event_collector.cpp
    browser/qt_webengine/devtools_panel.h
    browser/qt_webengine/devtools_panel.cpp
    browser/qt_webengine/tab_search_popup.h
    browser/qt_webengine/tab_search_popup.cpp
)
target_include_directories(aafr-browser-ui PUBLIC browser/qt_webengine)
target_link_libraries(aafr-browser-ui PUBLIC
    aafr-network-monitor
    aafr-network-state
    aafr-forwarding
    Qt6::Widgets
    Qt6::WebEngineWidgets
)
if(WIN32)
    target_link_libraries(aafr-browser-ui PRIVATE Qt6::GuiPrivate)
endif()

qt_add_executable(aafr-browser
    ${AAFR_BROWSER_RESOURCES}
    browser/qt_webengine/main.cpp
)
target_link_libraries(aafr-browser PRIVATE aafr-browser-ui)

enable_testing()

option(AAFR_BUILD_RUNTIME_TESTS "Build runtime and telemetry core tests" ON)
if(AAFR_BUILD_RUNTIME_TESTS)
    # V0.5 forwarding engine tests (no Internet access needed).
    foreach(v05_test IN ITEMS
            forwarding_graph:unit/forwarding_graph_test
            path_algorithms:unit/path_algorithms_test
            loopback_forwarding:unit/loopback_forwarding_test
            benchmark:unit/benchmark_test
            forwarding_pipeline:integration/forwarding_pipeline_test)
        string(REPLACE ":" ";" v05_parts "${v05_test}")
        list(GET v05_parts 0 v05_name)
        list(GET v05_parts 1 v05_source)
        string(REPLACE "_" "-" v05_target "${v05_name}-tests")
        add_executable(${v05_target} tests/${v05_source}.cpp)
        aafr_plain_cxx(${v05_target})
        target_link_libraries(${v05_target} PRIVATE aafr-forwarding)
        add_test(NAME ${v05_name} COMMAND ${v05_target})
        set_tests_properties(${v05_name} PROPERTIES TIMEOUT 120)
    endforeach()

    add_executable(runtime-core-smoke-test tests/unit/runtime_core_smoke_test.cpp)
    aafr_plain_cxx(runtime-core-smoke-test)
    target_link_libraries(runtime-core-smoke-test PRIVATE aafr-runtime)
    add_test(NAME runtime_core_smoke COMMAND runtime-core-smoke-test)

    add_executable(telemetry-core-tests tests/unit/telemetry_core_test.cpp)
    aafr_plain_cxx(telemetry-core-tests)
    target_link_libraries(telemetry-core-tests PRIVATE aafr-telemetry)
    add_test(NAME telemetry_core COMMAND telemetry-core-tests)
    set_tests_properties(telemetry_core PROPERTIES TIMEOUT 60)

    add_executable(network-state-tests tests/unit/network_state_test.cpp)
    aafr_plain_cxx(network-state-tests)
    target_link_libraries(network-state-tests PRIVATE aafr-network-state)
    add_test(NAME network_state COMMAND network-state-tests)
    set_tests_properties(network_state PROPERTIES TIMEOUT 60)

    add_executable(algorithm-eligibility-tests tests/unit/algorithm_eligibility_test.cpp)
    aafr_plain_cxx(algorithm-eligibility-tests)
    target_link_libraries(algorithm-eligibility-tests PRIVATE aafr-network-state)
    add_test(NAME algorithm_eligibility COMMAND algorithm-eligibility-tests)
    set_tests_properties(algorithm_eligibility PROPERTIES TIMEOUT 60)

    add_executable(state-pipeline-tests tests/integration/state_pipeline_test.cpp)
    aafr_plain_cxx(state-pipeline-tests)
    target_link_libraries(state-pipeline-tests PRIVATE aafr-network-state)
    add_test(NAME state_pipeline COMMAND state-pipeline-tests)
    set_tests_properties(state_pipeline PROPERTIES TIMEOUT 60)
endif()

option(AAFR_BUILD_BROWSER_TESTS "Build Qt browser interface tests" OFF)
if(AAFR_BUILD_BROWSER_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(browser-interface-tests
        ${AAFR_BROWSER_RESOURCES}
        tests/unit/browser_interface_test.cpp
    )
    target_link_libraries(browser-interface-tests PRIVATE
        aafr-browser-ui Qt6::Test Qt6::Network
    )
    add_test(NAME browser_interface COMMAND browser-interface-tests
        -o -,txt -o browser-interface-results.txt,txt)
    set_tests_properties(browser_interface PROPERTIES
        ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<SHELL_PATH:$<TARGET_FILE_DIR:Qt6::Core>>"
    )
    set_tests_properties(browser_interface PROPERTIES TIMEOUT 90)

    qt_add_executable(browser-observability-tests
        ${AAFR_BROWSER_RESOURCES}
        tests/unit/browser_observability_test.cpp
    )
    target_link_libraries(browser-observability-tests PRIVATE
        aafr-browser-ui Qt6::Test Qt6::Network
    )
    add_test(NAME browser_observability COMMAND browser-observability-tests
        -o -,txt -o browser-observability-results.txt,txt)
    set_tests_properties(browser_observability PROPERTIES
        ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<SHELL_PATH:$<TARGET_FILE_DIR:Qt6::Core>>"
    )
    # About 16 s when run directly. The first launch of a newly built unsigned
    # test program can be much slower under Windows reputation checks.
    set_tests_properties(browser_observability PROPERTIES TIMEOUT 240)
endif()
