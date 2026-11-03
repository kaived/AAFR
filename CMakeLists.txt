
cmake_minimum_required(VERSION 3.22)

project(AAFR VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

qt_standard_project_setup()
qt_add_resources(AAFR_BROWSER_RESOURCES browser/qt_webengine/browser_assets.qrc)

qt_add_executable(aafr-browser
    ${AAFR_BROWSER_RESOURCES}
    browser/qt_webengine/main.cpp
    browser/qt_webengine/browser_window.h
    browser/qt_webengine/browser_window.cpp
    browser/qt_webengine/tab_manager.h
    browser/qt_webengine/tab_manager.cpp
    browser/qt_webengine/browser_theme.h
    browser/qt_webengine/browser_theme.cpp
)

target_link_libraries(aafr-browser PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
)

option(AAFR_BUILD_BROWSER_TESTS "Build Qt browser interface tests" OFF)
if(AAFR_BUILD_BROWSER_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    qt_add_executable(browser-interface-tests
        ${AAFR_BROWSER_RESOURCES}
        tests/unit/browser_interface_test.cpp
        browser/qt_webengine/browser_window.h
        browser/qt_webengine/browser_window.cpp
        browser/qt_webengine/tab_manager.h
        browser/qt_webengine/tab_manager.cpp
        browser/qt_webengine/browser_theme.h
        browser/qt_webengine/browser_theme.cpp
    )
    target_include_directories(browser-interface-tests PRIVATE browser/qt_webengine)
    target_link_libraries(browser-interface-tests PRIVATE
        Qt6::Widgets Qt6::WebEngineWidgets Qt6::Test Qt6::Network
    )
    add_test(NAME browser_interface COMMAND browser-interface-tests
        -o -,txt -o browser-interface-results.txt,txt)
    set_tests_properties(browser_interface PROPERTIES
        ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<SHELL_PATH:$<TARGET_FILE_DIR:Qt6::Core>>"
    )
    set_tests_properties(browser_interface PROPERTIES TIMEOUT 90)
endif()
