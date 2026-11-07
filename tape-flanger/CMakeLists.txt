cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)

# ---- edit these for each plug-in ----
set(SP_TARGET  SmoothTapeFlanger)
set(SP_NAME    "Smooth Tape Flanger")
set(SP_CODE    STfl)
set(SP_BUNDLE  "com.lukegarito.smoothtapeflanger")
set(SP_DESC    "Through-zero tape style flanger")
set(SP_VERSION 1.1.1)
set(SP_AU_TYPE kAudioUnitType_Effect)   # kAudioUnitType_MusicEffect if it takes MIDI, kAudioUnitType_MusicDevice for an instrument
set(SP_SYNTH   FALSE)                   # TRUE only for an instrument
set(SP_MIDI_IN FALSE)                   # TRUE for MusicEffect and MusicDevice
set(SP_SOURCES src/PluginProcessor.cpp) # every .cpp except tests
# -------------------------------------

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

juce_add_plugin(${SP_TARGET}
    VERSION ${SP_VERSION}
    COMPANY_NAME "Luke Garito"
    BUNDLE_ID ${SP_BUNDLE}
    PLUGIN_MANUFACTURER_CODE Luke
    PLUGIN_CODE ${SP_CODE}
    FORMATS AU
    PRODUCT_NAME ${SP_NAME}
    PLUGIN_NAME ${SP_NAME}
    DESCRIPTION ${SP_DESC}
    AU_MAIN_TYPE ${SP_AU_TYPE}
    IS_SYNTH ${SP_SYNTH}
    NEEDS_MIDI_INPUT ${SP_MIDI_IN}
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)   # installed only by the InstallAU target below

target_sources(${SP_TARGET} PRIVATE ${SP_SOURCES})

set(SP_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_compile_definitions(${SP_TARGET} PUBLIC ${SP_DEFS})
target_link_libraries(${SP_TARGET}
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- install: cmake --build build --target InstallAU ----
set(AU_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
add_custom_target(InstallAU
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${SP_TARGET}_AU>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${AU_INSTALL_DIR}/${SP_NAME}.component"
    COMMAND ditto "$<TARGET_BUNDLE_DIR:${SP_TARGET}_AU>" "${AU_INSTALL_DIR}/${SP_NAME}.component"
    COMMENT "Ad-hoc signing and installing ${SP_NAME}.component"
    VERBATIM)
add_dependencies(InstallAU ${SP_TARGET}_AU)

# ---- offline test (not shipped): cmake --build build --target PluginTest -j 2 ; ctest --test-dir build ----
enable_testing()
juce_add_console_app(PluginTest PRODUCT_NAME "PluginTest")
target_sources(PluginTest PRIVATE ${SP_SOURCES} tests/PluginTest.cpp)
target_include_directories(PluginTest PRIVATE src tests)
target_compile_definitions(PluginTest PRIVATE ${SP_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(PluginTest PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME plugin COMMAND PluginTest)
