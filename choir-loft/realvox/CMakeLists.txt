# Real vocal measurement tool. Builds against any Choir Loft source tree, so two versions can be measured with the same analysis code.
#   cmake -S realvox -B <build dir> -DCHOIR_SRC=<Choir Loft source tree, default this repo>
#   cmake --build <build dir> -j 2
# The vocals themselves never go in this repo. See README.md in this folder.
cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
project(RealVox VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CHOIR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Choir Loft source tree whose processor renders")
set(JUCE_DIR "" CACHE PATH "A JUCE checkout to use (empty: download JUCE 8.0.10)")
if(JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.10 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_binary_data(RealVoxAssets
    HEADER_NAME WindowAssets.h
    NAMESPACE WindowAssets
    SOURCES "${CHOIR_SRC}/assets/fonts/Inter-Regular.otf" "${CHOIR_SRC}/assets/fonts/Inter-Medium.otf")

juce_add_console_app(RealVox PRODUCT_NAME "RealVox")
target_sources(RealVox PRIVATE
    RealVox.cpp
    "${CHOIR_SRC}/src/PluginProcessor.cpp"
    "${CHOIR_SRC}/src/editor/Theme.cpp"
    "${CHOIR_SRC}/src/editor/Controls.cpp"
    "${CHOIR_SRC}/src/editor/StageView.cpp"
    "${CHOIR_SRC}/src/editor/HarmonyEditor.cpp")
target_include_directories(RealVox PRIVATE "${CHOIR_SRC}/src")
target_compile_definitions(RealVox PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
                           JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(RealVox PRIVATE RealVoxAssets juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
