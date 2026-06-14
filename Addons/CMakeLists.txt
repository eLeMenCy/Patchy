cmake_minimum_required(VERSION 3.22)
project(NodeGraphAddons)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directory ──────────────────────────────────────────────────────────
# Built addons land in build/addons/ for easy testing.
# Copy them to your NodeGraph addons folder to load them.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")  # Windows .dll

# ── Helper macro ─────────────────────────────────────────────────────────────
macro(add_node_addon TARGET_NAME SOURCE_FILE)
    add_library(${TARGET_NAME} SHARED ${SOURCE_FILE})

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../AddonAPI")

    # No JUCE, no external dependencies — just the C++ standard library.
    # Remove the default "lib" prefix on Linux/macOS so the file is
    # "LevelAddon.so" not "libLevelAddon.so".
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")

    # On macOS, produce .dylib (not .so)
    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
endmacro()

# ── Example addons ───────────────────────────────────────────────────────────
add_node_addon(LevelAddon      LevelAddon/LevelAddon.cpp)
add_node_addon(TransposeAddon TransposeAddon/TransposeAddon.cpp)
add_node_addon(AmpAddon       AmpAddon/AmpAddon.cpp)
add_node_addon(EnvelopeAddon      EnvelopeAddon/EnvelopeAddon.cpp)
add_node_addon(StereoSplitterAddon  StereoSplitterAddon/StereoSplitterAddon.cpp)
add_node_addon(SpectrumyserAddon    SpectrumyserAddon/SpectrumyserAddon.cpp)

# ── Install target (optional) ─────────────────────────────────────────────────
# cmake --install build --prefix ~ will copy addons to the right folder.
if(APPLE)
    set(ADDON_INSTALL_DIR "$ENV{HOME}/Library/Patchy/Addons")
elseif(WIN32)
    set(ADDON_INSTALL_DIR "$ENV{APPDATA}/Patchy/Addons")
else()
    set(ADDON_INSTALL_DIR "$ENV{HOME}/.patchy/addons")
endif()

install(TARGETS LevelAddon TransposeAddon AmpAddon EnvelopeAddon
        LIBRARY DESTINATION "${ADDON_INSTALL_DIR}"
        RUNTIME DESTINATION "${ADDON_INSTALL_DIR}")
