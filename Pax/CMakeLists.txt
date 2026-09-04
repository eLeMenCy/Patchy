cmake_minimum_required(VERSION 3.22)
project(PatchyPax)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directory ──────────────────────────────────────────────────────────
# Built Pax land in build/pax/ for easy testing.
# Copy them to your Patchy Pax folder to load them.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pax")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pax")  # Windows .dll

# ── Helper macro ─────────────────────────────────────────────────────────────
macro(add_pax TARGET_NAME SOURCE_FILE)
    add_library(${TARGET_NAME} SHARED ${SOURCE_FILE})

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    # No JUCE, no external dependencies — just the C++ standard library.
    # Remove the default "lib" prefix on Linux/macOS so the file is
    # "LevelPax.dylib" not "libLevelPax.dylib".
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")

    # On macOS, produce .dylib (not .so)
    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
endmacro()

# ── Bundled Pax ───────────────────────────────────────────────────────────────
add_pax(LevelPax         LevelPax/LevelPax.cpp)
add_pax(TransposePax     TransposePax/TransposePax.cpp)
add_pax(AmpPax           AmpPax/AmpPax.cpp)
add_pax(EnvelopePax      EnvelopePax/EnvelopePax.cpp)
add_pax(StereoSplitterPax StereoSplitterPax/StereoSplitterPax.cpp)
add_pax(SpectrumyserPax  SpectrumyserPax/SpectrumyserPax.cpp)
add_pax(MqttToValuePax   MqttToValuePax/MqttToValuePax.cpp)
add_pax(OscToValuePax    OscToValuePax/OscToValuePax.cpp)
add_pax(ValueToDMXPax    ValueToDMXPax/ValueToDMXPax.cpp)
add_pax(AudioToDmxPax AudioToDmxPax/AudioToDmxPax.cpp)
add_pax(MidiToDmxPax     MidiToDmxPax/MidiToDmxPax.cpp)
add_pax(UdpValueToMidiCCPax UdpValueToMidiCCPax/UdpValueToMidiCCPax.cpp)
add_pax(AudioPeakToOscPax AudioPeakToOscPax/AudioPeakToOscPax.cpp)

# ── Install target (optional) ─────────────────────────────────────────────────
# cmake --install build --prefix ~ will copy Pax to the right folder.
if(APPLE)
    set(PAX_INSTALL_DIR "$ENV{HOME}/Library/Patchy/Pax")
elseif(WIN32)
    set(PAX_INSTALL_DIR "$ENV{APPDATA}/Patchy/Pax")
else()
    set(PAX_INSTALL_DIR "$ENV{HOME}/.patchy/pax")
endif()

install(TARGETS LevelPax TransposePax AmpPax EnvelopePax StereoSplitterPax SpectrumyserPax MqttToValuePax OscToValuePax ValueToDMXPax AudioToDmxPax MidiToDmxPax UdpValueToMidiCCPax AudioPeakToOscPax
        LIBRARY DESTINATION "${PAX_INSTALL_DIR}"
        RUNTIME DESTINATION "${PAX_INSTALL_DIR}")
