cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
project(SmoothResonance VERSION 0.1.0 LANGUAGES C CXX)
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

juce_add_plugin(SmoothResonance
    VERSION 0.1.0
    COMPANY_NAME "Luke Garito"
    BUNDLE_ID "com.lukegarito.smoothresonance"
    PLUGIN_MANUFACTURER_CODE Luke
    PLUGIN_CODE SmRs
    FORMATS AU
    PRODUCT_NAME "Smooth Resonance"
    PLUGIN_NAME "Smooth Resonance"
    DESCRIPTION "Dynamically turns down resonant peaks that stick out of the spectrum"
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)   # installed only by the InstallAU target below

target_sources(SmoothResonance PRIVATE
    src/ResonanceEngine.cpp
    src/PluginProcessor.cpp)

set(SR_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_compile_definitions(SmoothResonance PUBLIC ${SR_DEFS})
target_link_libraries(SmoothResonance
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- install: cmake --build build --target InstallAU ----
set(AU_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
add_custom_target(InstallAU
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:SmoothResonance_AU>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${AU_INSTALL_DIR}/Smooth Resonance.component"
    COMMAND ditto "$<TARGET_BUNDLE_DIR:SmoothResonance_AU>" "${AU_INSTALL_DIR}/Smooth Resonance.component"
    COMMENT "Ad-hoc signing and installing Smooth Resonance.component"
    VERBATIM)
add_dependencies(InstallAU SmoothResonance_AU)

# ---- offline tests (not shipped) ----
enable_testing()

juce_add_console_app(DspTest PRODUCT_NAME "DspTest")
target_sources(DspTest PRIVATE src/ResonanceEngine.cpp tests/DspTest.cpp)
target_include_directories(DspTest PRIVATE src)
target_compile_definitions(DspTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(DspTest PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME dsp COMMAND DspTest)

juce_add_console_app(SpeechTest PRODUCT_NAME "SpeechTest")
target_sources(SpeechTest PRIVATE src/ResonanceEngine.cpp tests/SpeechTest.cpp)
target_include_directories(SpeechTest PRIVATE src)
target_compile_definitions(SpeechTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0 SPEECH_DIR="${CMAKE_BINARY_DIR}/speech")
target_link_libraries(SpeechTest PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME speech COMMAND SpeechTest)

juce_add_console_app(ProcessorTest PRODUCT_NAME "ProcessorTest")
target_sources(ProcessorTest PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp tests/ProcessorTest.cpp)
target_include_directories(ProcessorTest PRIVATE src)
target_compile_definitions(ProcessorTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(ProcessorTest PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME processor COMMAND ProcessorTest)

# Loads the installed .component through the system Audio Unit API, the way a host does. Run after InstallAU.
add_executable(HostCheck tests/HostCheck.cpp)
target_link_libraries(HostCheck PRIVATE "-framework AudioToolbox" "-framework CoreFoundation" "-framework Accelerate")
