cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)

# Switchboard: the link member (link/SmoothLink.h), the Switchboard AU (hub/) and the tools that check them (tools/).
# Build: nice -n 15 cmake --build build -j 2 ; tests: ctest --test-dir build --output-on-failure
set(SP_TARGET  Switchboard)
set(SP_NAME    "Switchboard")
set(SP_CODE    SSwb)
set(SP_BUNDLE  "com.lukegarito.switchboard")
set(SP_DESC    "Shows and sets the controls of your plug-ins on this track")
set(SP_VERSION 0.1.0)

project(${SP_TARGET} VERSION ${SP_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# JUCE: a checkout at JUCE_DIR if you give one, else JUCE 8.0.10 is downloaded. Everything it generates lands in this build folder.
set(JUCE_DIR "" CACHE PATH "A JUCE checkout to use (empty: download JUCE 8.0.10)")
if(JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.10 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(SP_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# Every test runs against scratch folders, never Luke's real link folder or key.json.
set(SCRATCH "${CMAKE_SOURCE_DIR}/work/scratch")
set(TEST_ENV "SMOOTH_LINK_DIR=${SCRATCH}/linktest;SMOOTH_KEY_JSON=${SCRATCH}/key.json")

enable_testing()

# ---- Task 1: the link member ----
juce_add_console_app(LinkTest PRODUCT_NAME "LinkTest")
target_sources(LinkTest PRIVATE tests/LinkTest.cpp)
target_include_directories(LinkTest PRIVATE link tests)
target_compile_definitions(LinkTest PRIVATE ${SP_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(LinkTest PRIVATE juce::juce_audio_processors PUBLIC juce::juce_recommended_config_flags)
add_test(NAME link COMMAND LinkTest)
set_tests_properties(link PROPERTIES ENVIRONMENT "${TEST_ENV}")

# A stand-in plug-in instance with a link, under a real message loop. For end-to-end tests of link readers.
juce_add_console_app(LinkStub PRODUCT_NAME "LinkStub")
target_sources(LinkStub PRIVATE tools/LinkStub.cpp)
target_include_directories(LinkStub PRIVATE link tests)
target_compile_definitions(LinkStub PRIVATE ${SP_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(LinkStub PRIVATE juce::juce_audio_processors PUBLIC juce::juce_recommended_config_flags)
