cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
project(SmoothResonance VERSION 0.7.1 LANGUAGES C CXX)
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
    VERSION 0.7.1
    COMPANY_NAME "Luke Garito"
    BUNDLE_ID "com.lukegarito.smoothresonance"
    PLUGIN_MANUFACTURER_CODE Luke
    PLUGIN_CODE SmRs
    FORMATS AU
    PRODUCT_NAME "Velvet Rope"
    PLUGIN_NAME "Velvet Rope"
    DESCRIPTION "Dynamically turns down resonant peaks that stick out of the spectrum"
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)   # installed only by the InstallAU target below

# The window: one embedded font family (SIL OFL, see assets/fonts/OFL.txt) and the drawing code.
juce_add_binary_data(SmoothResonanceWindowAssets
    HEADER_NAME WindowAssets.h
    NAMESPACE WindowAssets
    SOURCES
        assets/fonts/Inter-Regular.otf
        assets/fonts/Inter-Medium.otf)

set(SR_WINDOW_SOURCES
    src/editor/Theme.cpp
    src/editor/Controls.cpp
    src/editor/ResonanceGraph.cpp
    src/editor/ResonanceEditor.cpp)

target_sources(SmoothResonance PRIVATE
    src/ResonanceEngine.cpp
    src/PluginProcessor.cpp
    ${SR_WINDOW_SOURCES})

set(SR_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_compile_definitions(SmoothResonance PUBLIC ${SR_DEFS})
target_link_libraries(SmoothResonance
    PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- install: cmake --build build --target InstallAU ----
set(AU_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
add_custom_target(InstallAU
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:SmoothResonance_AU>"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${AU_INSTALL_DIR}/Velvet Rope.component"
    COMMAND ditto "$<TARGET_BUNDLE_DIR:SmoothResonance_AU>" "${AU_INSTALL_DIR}/Velvet Rope.component"
    COMMENT "Ad-hoc signing and installing Velvet Rope.component"
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
target_sources(ProcessorTest PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp ${SR_WINDOW_SOURCES} tests/ProcessorTest.cpp)
target_include_directories(ProcessorTest PRIVATE src)
target_compile_definitions(ProcessorTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(ProcessorTest PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME processor COMMAND ProcessorTest)

# Old sessions keep their sound: nulls against saved 0.4.1 renders and a frozen copy of the 0.4.1 engine.
juce_add_console_app(NullTest PRODUCT_NAME "NullTest")
target_sources(NullTest PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp ${SR_WINDOW_SOURCES} tests/legacy/LegacyEngine.cpp tests/NullTest.cpp)
target_include_directories(NullTest PRIVATE src tests)
target_compile_definitions(NullTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0 BASELINE_DIR="${CMAKE_SOURCE_DIR}/tests/baseline" SPEECH_DIR="${CMAKE_BINARY_DIR}/speech")
target_link_libraries(NullTest PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME null COMMAND NullTest)

# The five features of 0.5.1, each measured.
juce_add_console_app(FeatureTest PRODUCT_NAME "FeatureTest")
target_sources(FeatureTest PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp ${SR_WINDOW_SOURCES} tests/legacy/LegacyEngine.cpp tests/FeatureTest.cpp)
target_include_directories(FeatureTest PRIVATE src tests)
target_compile_definitions(FeatureTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0 SPEECH_DIR="${CMAKE_BINARY_DIR}/speech")
target_link_libraries(FeatureTest PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME features COMMAND FeatureTest)

# Held Notes, added in 0.6.0: off is the old sound, on takes down a held note's ring. All through the real processor.
juce_add_console_app(HeldNotesTest PRODUCT_NAME "HeldNotesTest")
target_sources(HeldNotesTest PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp ${SR_WINDOW_SOURCES} tests/legacy/LegacyEngine.cpp tests/HeldNotesTest.cpp)
target_include_directories(HeldNotesTest PRIVATE src tests)
target_compile_definitions(HeldNotesTest PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0 BASELINE_DIR="${CMAKE_SOURCE_DIR}/tests/baseline")
target_link_libraries(HeldNotesTest PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
add_test(NAME heldnotes COMMAND HeldNotesTest)

# Renders the window to PNG with real speech running through the real processor. Not a ctest: it writes docs/window.
juce_add_console_app(RenderWindow PRODUCT_NAME "RenderWindow")
target_sources(RenderWindow PRIVATE src/ResonanceEngine.cpp src/PluginProcessor.cpp ${SR_WINDOW_SOURCES} tests/RenderWindow.cpp)
target_include_directories(RenderWindow PRIVATE src tests)
target_compile_definitions(RenderWindow PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 SPEECH_DIR="${CMAKE_BINARY_DIR}/speech")
target_link_libraries(RenderWindow PRIVATE SmoothResonanceWindowAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)

# Loads the installed .component through the system Audio Unit API, the way a host does. Run after InstallAU.
add_executable(HostCheck tests/HostCheck.cpp)
target_link_libraries(HostCheck PRIVATE "-framework AudioToolbox" "-framework CoreFoundation" "-framework Accelerate")

# The same, once per channel layout: channel order, the LFE from a real layout tag, and Quality's latency. Run after InstallAU.
add_executable(LayoutCheck tests/LayoutCheck.cpp)
target_link_libraries(LayoutCheck PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")

# Not a test: prints what the engine sees at one spot of a recording, and how deeply a list of rings is cut. See tests/RingProbe.cpp.
juce_add_console_app(RingProbe PRODUCT_NAME "RingProbe")
target_sources(RingProbe PRIVATE src/ResonanceEngine.cpp tests/RingProbe.cpp)
target_include_directories(RingProbe PRIVATE src)
target_compile_definitions(RingProbe PRIVATE ${SR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(RingProbe PRIVATE juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
