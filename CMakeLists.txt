cmake_minimum_required(VERSION 3.22)

# Statically link the MSVC runtime so the plug-in does not depend on the
# "Visual C++ Redistributable" being installed on the machine running the DAW.
# (Ignored by non-MSVC compilers.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(NorthstarMastering VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(JUCE)

set(NORTHSTAR_FORMATS VST3 Standalone)
set(NORTHSTAR_VST2_SDK_PATH "" CACHE PATH "Optional legacy VST2 SDK path")
if(NORTHSTAR_VST2_SDK_PATH)
    juce_set_vst2_sdk_path("${NORTHSTAR_VST2_SDK_PATH}")
    list(APPEND NORTHSTAR_FORMATS VST)
endif()

juce_add_plugin(NorthstarMastering
    COMPANY_NAME "Northstar Audio"
    BUNDLE_ID "audio.northstar.mastering"
    PLUGIN_MANUFACTURER_CODE Nsta
    PLUGIN_CODE NsM1
    PRODUCT_NAME "Northstar Master"
    DESCRIPTION "Adaptive mastering assistant"
    FORMATS ${NORTHSTAR_FORMATS}
    VST3_CATEGORIES Fx Mastering
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(NorthstarMastering)

target_sources(NorthstarMastering
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
)

target_compile_definitions(NorthstarMastering
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(NorthstarMastering
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)