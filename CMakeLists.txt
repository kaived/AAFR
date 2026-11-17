
cmake_minimum_required(VERSION 3.22)

project(AAFR VERSION 0.6.0 LANGUAGES CXX)

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

# V0.6 adaptive runtime: change detection, deterministic recommendation,
# controlled probing, statistical arbitration, stability gates, confirmed
# activation, recovery, DecisionRecords and outcome observation. Plain C++17,
# no Qt, no ML.
add_library(aafr-adaptive-runtime STATIC
    runtime/adaptive/runtime_clock.h
    runtime/adaptive/reason_codes.h
    runtime/adaptive/reason_codes.cpp
    runtime/adaptive/runtime_types.h
    runtime/adaptive/runtime_types.cpp
    runtime/adaptive/network_change_detector.h
    runtime/adaptive/network_change_detector.cpp
    runtime/adaptive/model_trace.h
    runtime/adaptive/model_trace.cpp
    runtime/adaptive/recommendation_provider.h
    runtime/adaptive/recommendation_provider.cpp
    runtime/adaptive/controlled_probe_engine.h
    runtime/adaptive/controlled_probe_engine.cpp
    runtime/adaptive/statistical_arbitration.h
    runtime/adaptive/statistical_arbitration.cpp
    runtime/adaptive/performance_evaluator.h
    runtime/adaptive/performance_evaluator.cpp
    runtime/adaptive/stability_controller.h
    runtime/adaptive/stability_controller.cpp
    runtime/adaptive/decision_record.h
    runtime/adaptive/decision_record.cpp
    runtime/adaptive/outcome_observer.h
    runtime/adaptive/outcome_observer.cpp
    runtime/adaptive/aafr_runtime.h
    runtime/adaptive/aafr_runtime.cpp
    runtime/observability/runtime_contracts.h
)
aafr_plain_cxx(aafr-adaptive-runtime)
target_link_libraries(aafr-adaptive-runtime PUBLIC aafr-forwarding)

# V0.7.2 expert-model integration: the persistent local Python inference
# worker client (JSON Lines over pipes), the model feature adapter, the
# canonical algorithm-label mapping and the AIRecommendationProvider. Plain
# C++17, no Qt: the runtime calls providers on its own worker thread. The
# model itself stays in Python (expert_model/inference/aafr_model_worker.py
# running the model project's unchanged ml/inference.py).
set(AAFR_MODEL_PYTHON "" CACHE FILEPATH "Python interpreter for the expert-model worker (needs numpy, pandas, scikit-learn, lightgbm, joblib)")
if(NOT AAFR_MODEL_PYTHON)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
        set(AAFR_MODEL_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe" CACHE FILEPATH "" FORCE)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
        set(AAFR_MODEL_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "" FORCE)
    endif()
endif()
set(AAFR_MODEL_DIR "" CACHE PATH "Expert-model release root containing models/ and ml/ (artifacts are verified against the manifest)")
set(AAFR_MODEL_WORKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/expert_model/inference/aafr_model_worker.py")
set(AAFR_MODEL_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/expert_model/models/model_manifest.json")

add_library(aafr-expert-model STATIC
    runtime/model/json_value.h
    runtime/model/json_value.cpp
    runtime/model/child_process.h
    runtime/model/child_process.cpp
    runtime/model/model_contract.h
    runtime/model/model_contract.cpp
    runtime/model/model_events.h
    runtime/model/model_events.cpp
    runtime/model/model_feature_adapter.h
    runtime/model/model_feature_adapter.cpp
    runtime/model/model_worker_client.h
    runtime/model/model_worker_client.cpp
    runtime/model/ai_recommendation_provider.h
    runtime/model/ai_recommendation_provider.cpp
)
aafr_plain_cxx(aafr-expert-model)
target_link_libraries(aafr-expert-model PUBLIC aafr-adaptive-runtime)
target_compile_definitions(aafr-expert-model PRIVATE
    AAFR_DEFAULT_MODEL_PYTHON="${AAFR_MODEL_PYTHON}"
    AAFR_DEFAULT_MODEL_WORKER="${AAFR_MODEL_WORKER_SCRIPT}"
    AAFR_DEFAULT_MODEL_DIR="${AAFR_MODEL_DIR}"
    AAFR_DEFAULT_MODEL_MANIFEST="${AAFR_MODEL_MANIFEST}"
)

# Controlled comparisons of fixed vs adaptive forwarding (V0.6 section 19).
add_executable(aafr-runtime-experiment tools/runtime_experiment/runtime_experiment_main.cpp)
aafr_plain_cxx(aafr-runtime-experiment)
target_link_libraries(aafr-runtime-experiment PRIVATE aafr-adaptive-runtime)

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
    aafr-adaptive-runtime
    aafr-expert-model
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

    # V0.6 adaptive runtime tests (loopback only, no Internet access needed).
    foreach(v06_test IN ITEMS
            runtime_change_detection:unit/runtime_change_detection_test
            runtime_statistics:unit/runtime_statistics_test
            runtime_stability:unit/runtime_stability_test
            runtime_recommendation:unit/runtime_recommendation_test
            adaptive_runtime:integration/adaptive_runtime_test
            runtime_loopback:integration/runtime_loopback_test
            # V0.7.1 external-runtime integration regressions.
            runtime_probe:unit/runtime_probe_test
            runtime_scoring:unit/runtime_scoring_test
            runtime_scenarios:integration/runtime_scenarios_test)
        string(REPLACE ":" ";" v06_parts "${v06_test}")
        list(GET v06_parts 0 v06_name)
        list(GET v06_parts 1 v06_source)
        string(REPLACE "_" "-" v06_target "${v06_name}-tests")
        add_executable(${v06_target} tests/${v06_source}.cpp)
        aafr_plain_cxx(${v06_target})
        target_link_libraries(${v06_target} PRIVATE aafr-adaptive-runtime)
        add_test(NAME ${v06_name} COMMAND ${v06_target})
        set_tests_properties(${v06_name} PROPERTIES TIMEOUT 180)
    endforeach()

    # V0.7.2 expert-model integration tests. The real-model cases use the
    # configured interpreter and model directory; without them they report
    # SKIP (never a pass of an unexecuted check).
    foreach(v072_test IN ITEMS
            model_contract:unit/model_contract_test
            model_feature_adapter:unit/model_feature_adapter_test
            model_ai_provider:unit/model_ai_provider_test
            model_ai_runtime:integration/model_ai_runtime_test
            model_worker:integration/model_worker_test
            model_e2e:integration/model_e2e_test)
        string(REPLACE ":" ";" v072_parts "${v072_test}")
        list(GET v072_parts 0 v072_name)
        list(GET v072_parts 1 v072_source)
        string(REPLACE "_" "-" v072_target "${v072_name}-tests")
        add_executable(${v072_target} tests/${v072_source}.cpp)
        aafr_plain_cxx(${v072_target})
        target_link_libraries(${v072_target} PRIVATE aafr-expert-model)
        target_compile_definitions(${v072_target} PRIVATE
            AAFR_TEST_MODEL_PYTHON="${AAFR_MODEL_PYTHON}"
            AAFR_TEST_MODEL_WORKER="${AAFR_MODEL_WORKER_SCRIPT}"
            AAFR_TEST_MODEL_DIR="${AAFR_MODEL_DIR}"
            AAFR_TEST_MODEL_MANIFEST="${AAFR_MODEL_MANIFEST}"
            AAFR_TEST_FAKE_WORKER="${CMAKE_CURRENT_SOURCE_DIR}/tests/model/fake_model_worker.py")
        add_test(NAME ${v072_name} COMMAND ${v072_target})
        set_tests_properties(${v072_name} PROPERTIES TIMEOUT 300)
    endforeach()
    if(AAFR_MODEL_PYTHON)
        add_test(NAME model_worker_python
            COMMAND ${AAFR_MODEL_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/model/test_model_worker.py
                    --worker ${AAFR_MODEL_WORKER_SCRIPT} --manifest ${AAFR_MODEL_MANIFEST} --model-dir "${AAFR_MODEL_DIR}")
        set_tests_properties(model_worker_python PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    endif()

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
    # V0.6 adds an end-to-end runtime switching test (about 1-2 minutes).
    set_tests_properties(browser_observability PROPERTIES TIMEOUT 480)
endif()
