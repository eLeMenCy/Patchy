cmake_minimum_required(VERSION 3.22)
project(Patchy VERSION 0.0.877)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(NODEGRAPH_DEV_MODE "Skip UI build, use Vite dev server on localhost:5173" OFF)

# ── Find npm ──────────────────────────────────────────────────────────────────
if(NOT NODEGRAPH_DEV_MODE)
    if(WIN32)
        find_program(NPM_EXE NAMES npm.cmd npm REQUIRED DOC "npm")
    else()
        find_program(NPM_EXE NAMES npm REQUIRED DOC "npm")
    endif()
    message(STATUS "npm: ${NPM_EXE}")
endif()

# ── Fetch JUCE 8 ──────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ── Build & zip React UI ──────────────────────────────────────────────────────
set(UI_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/UI")
set(UI_DIST  "${UI_DIR}/dist")
set(UI_ZIP   "${CMAKE_CURRENT_BINARY_DIR}/ui_assets.zip")
set(UI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/ui.stamp")

if(NOT NODEGRAPH_DEV_MODE)
    file(GLOB_RECURSE UI_SOURCES
        "${UI_DIR}/src/*" "${UI_DIR}/index.html"
        "${UI_DIR}/package.json" "${UI_DIR}/vite.config.ts" "${UI_DIR}/tsconfig.json")

    set(_need_build FALSE)
    if(NOT EXISTS "${UI_STAMP}" OR NOT EXISTS "${UI_DIST}/index.html")
        set(_need_build TRUE)
    else()
        foreach(_f ${UI_SOURCES})
            if("${_f}" IS_NEWER_THAN "${UI_STAMP}")
                set(_need_build TRUE)
                break()
            endif()
        endforeach()
    endif()

    if(_need_build)
        message(STATUS "Patchy: npm install…")
        execute_process(COMMAND ${NPM_EXE} install
            WORKING_DIRECTORY "${UI_DIR}" RESULT_VARIABLE r COMMAND_ECHO STDOUT)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "npm install failed")
        endif()
        message(STATUS "Patchy: npm run build…")
        execute_process(COMMAND ${NPM_EXE} run build
            WORKING_DIRECTORY "${UI_DIR}" RESULT_VARIABLE r COMMAND_ECHO STDOUT)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "npm run build failed")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
            WORKING_DIRECTORY "${UI_DIST}" RESULT_VARIABLE r)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "zip packaging failed")
        endif()
        file(WRITE "${UI_STAMP}" "")
        message(STATUS "Patchy: UI ready.")
    else()
        message(STATUS "Patchy: UI up-to-date.")
        if(NOT EXISTS "${UI_ZIP}")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
                WORKING_DIRECTORY "${UI_DIST}")
        endif()
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UI_SOURCES})

    juce_add_binary_data(PatchyUI
        HEADER_NAME "BinaryData.h"
        NAMESPACE   BinaryData
        SOURCES     "${UI_ZIP}")
endif()

# ── Plugin target ─────────────────────────────────────────────────────────────
juce_add_plugin(Patchy
    VERSION                  "0.0.877"
    COMPANY_NAME             "eLeMenCy"
    PLUGIN_MANUFACTURER_CODE "ELMc"
    PLUGIN_CODE              "NGrp"
    FORMATS                  VST3 AU Standalone
    PRODUCT_NAME             "Patchy"
    BUNDLE_ID                "com.elemency.patchy"
    IS_SYNTH                 TRUE          # produces audio from MIDI
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        TRUE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES          "Instrument" "Fx"
    AU_MAIN_TYPE             "kAudioUnitType_MusicDevice"
    COPY_PLUGIN_AFTER_BUILD  TRUE
)

target_sources(Patchy PRIVATE
    Source/PatchyProcessor.cpp
    Source/PatchyEditor.cpp
    Source/ProcessingGraph.cpp
    Source/NodeProcessor.cpp
    Addons/AddonRegistry.cpp
    Addons/AddonScanner.cpp
    Source/GraphModel.cpp
    Source/WebBridge.cpp
    Source/StandaloneApp.cpp
    Source/MidiDeviceNodes.cpp
    Source/AudioDeviceNodes.cpp
    Source/MidiMonitorNode.cpp
    Source/AudioMonitorNode.cpp
    Source/MidiKeyboardNode.cpp
)

if(NOT NODEGRAPH_DEV_MODE)
    target_link_libraries(Patchy PRIVATE PatchyUI)
    target_compile_definitions(Patchy PRIVATE HAS_BUNDLED_UI=1)
else()
    target_compile_definitions(Patchy PRIVATE HAS_BUNDLED_UI=0)
endif()

target_link_libraries(Patchy
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions(Patchy PRIVATE
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_WIN_WEBVIEW2=1
)

# ── Example addons (optional, built separately)
add_subdirectory(Addons EXCLUDE_FROM_ALL)

# ── Convenience targets ───────────────────────────────────────────────────────
if(NPM_EXE)
    add_custom_target(ui_rebuild
        COMMAND ${NPM_EXE} install
        COMMAND ${NPM_EXE} run build
        COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
        WORKING_DIRECTORY "${UI_DIR}"
        COMMENT "Rebuilding and repackaging React UI" VERBATIM)

    add_custom_target(ui_dev
        COMMAND ${NPM_EXE} run dev
        WORKING_DIRECTORY "${UI_DIR}"
        COMMENT "Starting Vite dev server" VERBATIM)
endif()

# ── Unit Tests ────────────────────────────────────────────────────────────────
option (PATCHY_BUILD_TESTS "Build Patchy unit tests" ON)

if (PATCHY_BUILD_TESTS)
    add_executable (PatchyTests
        Tests/PatchyTestRunner.cpp
        Tests/GraphModelTests.cpp
        Source/GraphModel.cpp
    )

    target_include_directories (PatchyTests PRIVATE Source Addons)

    target_compile_features (PatchyTests PRIVATE cxx_std_20)

    target_link_libraries (PatchyTests PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_gui_basics
    )

    target_compile_definitions (PatchyTests PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    enable_testing()
    add_test (NAME PatchyTests COMMAND PatchyTests)
endif()
