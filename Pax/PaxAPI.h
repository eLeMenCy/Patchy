// Copyright (c) 2024-2025 eLeMenCy
// See Pax/LICENSE for details.
//
// Pax developers are free to use this header under the MIT license
// and may license their own Pax code under any terms they choose.

/**
 * @file PaxAPI.h
 * @brief Patchy Pax API v4 (PAX) — the only header a Pax author needs.
 *
 * No JUCE dependency. No Patchy source dependency.
 *
 * Build your Pax as a shared library:
 *   macOS:   clang++ -std=c++17 -shared -fPIC MyNode.cpp -o MyNode.dylib
 *   Linux:   g++     -std=c++17 -shared -fPIC MyNode.cpp -o MyNode.so
 *   Windows: cl /std:c++17 /LD MyNode.cpp /Fe:MyNode.dll
 *
 * Drop the binary into the Patchy Pax folder and restart.
 *
 * @par API v2 changes (NGA_ → PAX_)
 *  - All symbols renamed from NGA_ to PAX_
 *  - PAX_process now receives a PAX_ProcessContext* instead of individual
 *    parameters — the signature will never grow again; future data types
 *    simply add fields to PAX_ProcessContext
 *  - PAX_Value + PAX_ValueBuffer added for DMX, OSC, MQTT, UDP and other
 *    protocol data types — value ports are live, routed by the host the
 *    same way audio/MIDI are (see valuesIn/valuesOut below)
 *  - PAX_API_VERSION bumped to 2
 *
 * @par API v3 changes
 *  - PAX_Value gained a `portIndex` field — lets a Pax with more than one
 *    Value output port (see PAX_getValueOutputCount/OutputType) say which
 *    specific port a written value belongs to, instead of every value
 *    implicitly going to "the" output. Needed for the host to route values
 *    to the correct downstream connection when a node has several
 *    differently-typed output ports (see PaxPortSpec in the host source for
 *    the design story) — mirrors how audio already routes per-port via
 *    dedicated buffer slots. 0 = first declared output port, the default
 *    if never set — correct as-is for every existing single-output Pax,
 *    no source change needed, just a rebuild (this is a genuine struct
 *    size change, hence the version bump — an un-rebuilt v2 Pax is cleanly
 *    rejected by the host's existing apiVersion check, not silently
 *    miscompiled against the new layout)
 *  - PAX_API_VERSION bumped to 3
 *
 * @par API v4 changes
 *  - PAX_ProcessContext gained a separate DMX universe path (dmxFrameIn/
 *    dmxFrameOut/dmxFrameInValid/dmxFrameOutValid) — PAX_Value.data[] is
 *    only 56 bytes, nowhere near enough for a full 512-channel DMX universe,
 *    which was silently truncating any Pax or built-in node trying to
 *    address more than the first 56 channels through the general Value
 *    mechanism. This is a parallel path, not a replacement — Value ports
 *    (valuesIn/valuesOut) are untouched and still the right choice for
 *    MQTT/OSC/UDP/single-value DMX; use the DMX frame fields specifically
 *    when a Pax needs to read or write the full 512-channel universe.
 *  - PAX_API_VERSION bumped to 4
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def PAX_API_VERSION
 * @brief The host rejects any Pax built against a different major version —
 *        must match exactly, not just be "close enough".
 */
#define PAX_API_VERSION 4

/**
 * @brief Opaque instance handle.
 *
 * Allocate whatever internal struct you need and cast its pointer to this.
 */
typedef void PAX_Instance;

/**
 * @brief A single MIDI event.
 *
 * Messages are passed as an array of PAX_MidiEvent structs.
 */
typedef struct {
    int32_t sampleOffset;   ///< Sample position within the current block (0 … numSamples-1)
    uint8_t byteCount;      ///< Number of valid bytes (1–3)
    uint8_t bytes[3];       ///< Raw MIDI bytes, e.g. { 0x90, 60, 100 } = note-on C4 vel 100
} PAX_MidiEvent;

/**
 * @brief Universal non-audio/MIDI payload (new in API v2).
 *
 * Carries a single value event for any non-audio/MIDI protocol:
 * DMX channel values, OSC messages, MQTT payloads, UDP data, etc.
 *
 * @par Primary path (99% of use cases)
 * Set dataType = PAX_DATA_FLOAT and use the `value` field.
 * e.g. a DMX channel level, an OSC float, a MQTT numeric value.
 *
 * @par Extended path (strings, blobs)
 * Set dataType = PAX_DATA_STRING or PAX_DATA_BLOB,
 * set dataSize to the byte length, and write into data[].
 * data[] is 56 bytes inline — no heap allocation for short payloads
 * (covers typical MQTT topics, OSC address paths, short byte arrays).
 */

/// @name Domain type tags — which protocol a PAX_Value belongs to
/// @{
#define PAX_TYPE_GENERIC  0   ///< Unspecified / generic float value
#define PAX_TYPE_DMX      1   ///< DMX512 channel value
#define PAX_TYPE_OSC      2   ///< OSC message
#define PAX_TYPE_MQTT     3   ///< MQTT payload
#define PAX_TYPE_UDP      4   ///< Raw UDP datagram
#define PAX_TYPE_ARTNET   5   ///< ArtNet DMX universe
// 6–255 reserved for future protocol types
/// @}

/// @name Data type tags — what's actually in the payload
/// @{
#define PAX_DATA_FLOAT    0   ///< Use `value` field (default)
#define PAX_DATA_STRING   1   ///< Use data[] as a null-terminated string
#define PAX_DATA_BLOB     2   ///< Use data[] as raw bytes, length = dataSize
/// @}

typedef struct {
    uint32_t key;        ///< Integer key — resolved from a name at setup time,
                          ///< never a string at runtime (avoids hashing on the audio thread)
    uint8_t  type;        ///< Domain tag: PAX_TYPE_*
    uint8_t  dataType;    ///< Payload type: PAX_DATA_*
    uint16_t dataSize;    ///< Byte length of data[] when dataType != PAX_DATA_FLOAT
    float    value;       ///< Primary payload (PAX_DATA_FLOAT, default)
    uint8_t  data[56];    ///< Inline buffer for strings/blobs (no heap alloc)
    /**
     * Which declared Value output port this belongs to (API v3). 0 = first
     * port, the correct default for any Pax with only one Value output —
     * only matters once PAX_getValueOutputCount() > 1. Host
     * zero-initializes valuesOut[] each block, so an unset portIndex is
     * always a safe 0. Total struct size: 69 bytes (was 68 in API v2).
     */
    uint8_t  portIndex;
} PAX_Value;

/**
 * @brief All per-block data in one struct (new in API v2).
 *
 * Passed by pointer to PAX_process(). Future data types add fields here;
 * the PAX_process signature itself never changes.
 *
 * Fields marked [placeholder] are reserved for future use — the host
 * passes NULL / 0 until the corresponding feature is implemented.
 * Pax should check for NULL before accessing these fields.
 */
typedef struct {
    /// @name Audio
    /// @{
    float**  audioIn;        ///< [numChannels] input channel pointers
    float**  audioOut;       ///< [numChannels] output channel pointers
    int      numChannels;    ///< Always 2 (stereo) in current version
    int      numSamples;     ///< Block size (≤ maxBlockSize from PAX_prepare)
    /// @}

    /// @name MIDI
    /// @{
    const PAX_MidiEvent* midiIn;       ///< Input events (may be NULL)
    int                  midiInCount;  ///< Number of input events
    PAX_MidiEvent*       midiOut;      ///< Write output events here
    int*                 midiOutCount; ///< Set to number of events written
    int                  midiMaxCount; ///< Max events you may write to midiOut
    /// @}

    /**
     * @name Values (PAX_Value)
     * Value events flow through the graph between Pax nodes that have
     * value ports. Guard against NULL for forward compatibility with
     * future host versions.
     */
    /// @{
    const PAX_Value* valuesIn;       ///< Input value events (may be NULL)
    int              valueInCount;   ///< Number of input value events
    PAX_Value*       valuesOut;      ///< Write output value events here (may be NULL)
    int*             valueOutCount;  ///< Set to number of value events written
    int              valueMaxCount;  ///< Max value events you may write
    /// @}

    /**
     * @name DMX universe (API v4)
     * A separate wide-payload path from Values above — PAX_Value.data[] is
     * only 56 bytes, nowhere near enough for a full 512-channel universe.
     * Single slot per direction (today's convention: one DMX universe per
     * node — see the host's DmxDeviceNodes.h). Always 512 bytes when
     * non-NULL. Host zero-fills dmxFrameOut and clears *dmxFrameOutValid
     * before each block, so you only need to touch the channel(s) you
     * actually write, not the whole 512.
     */
    /// @{
    const uint8_t* dmxFrameIn;        ///< Input universe, 512 bytes (may be NULL)
    bool           dmxFrameInValid;   ///< true if dmxFrameIn holds real data this block
    uint8_t*       dmxFrameOut;       ///< Write up to 512 channel bytes here (may be NULL)
    bool*          dmxFrameOutValid;  ///< Set *dmxFrameOutValid = true if you wrote a frame
    /// @}

} PAX_ProcessContext;

/**
 * @brief Static metadata — no instance required.
 */
typedef struct {
    const char* name;          ///< Display name  e.g. "Gain", "Transpose"
    const char* vendor;        ///< Author/studio e.g. "ACME Audio"
    const char* version;       ///< Semver string e.g. "1.0.0"
    /**
     * 1 = MIDI only
     * 2 = Audio only
     * 3 = MIDI + Audio (AV)
     * 4 = Value only (no audio, no MIDI — cross-protocol
     *     adapters/converters; port counts come from
     *     PAX_getValueInputCount/PAX_getValueOutputCount below, NOT from
     *     this descriptor, to avoid an ABI-breaking struct change for
     *     already-compiled Pax binaries)
     */
    int         nodeType;
    int         apiVersion;    ///< Must equal PAX_API_VERSION

    /**
     * @name Optional port counts (0 = use nodeType defaults)
     * Set these to override the default 1-in / 1-out layout.
     * e.g. a stereo splitter: audioInputs=1, audioOutputs=2
     */
    /// @{
    int         audioInputs;   ///< Number of audio input ports  (0 = default)
    int         audioOutputs;  ///< Number of audio output ports (0 = default)
    int         midiInputs;    ///< Number of MIDI input ports   (0 = default)
    int         midiOutputs;   ///< Number of MIDI output ports  (0 = default)
    /// @}
} PAX_Descriptor;

/**
 * @brief Describes one exposed parameter.
 */
typedef struct {
    const char* name;          ///< "Gain", "Semitones" …
    float       minValue;      ///< Minimum value
    float       maxValue;      ///< Maximum value
    float       defaultValue;  ///< Default value
    float       step;          ///< 0 = continuous, 1 = integer steps, etc.
} PAX_ParameterInfo;

/**
 * @name Required exports
 * Every Pax MUST provide all of these.
 */
/// @{

/**
 * @brief Return a pointer to a static PAX_Descriptor.
 *
 * Called once at scan time. Must never return NULL.
 * @return Pointer to a static, never-changing PAX_Descriptor.
 */
const PAX_Descriptor* PAX_getDescriptor (void);

/**
 * @brief Create a new processing instance.
 *
 * Called on the message thread.
 * @return A new instance, or NULL on failure.
 */
PAX_Instance* PAX_create (void);

/**
 * @brief Destroy an instance previously returned by PAX_create().
 * @param instance The instance to destroy.
 */
void PAX_destroy (PAX_Instance* instance);

/**
 * @brief Called before processing starts or when host settings change.
 *
 * Called on the MESSAGE thread — safe to allocate here.
 * Will always be called before the first PAX_process().
 * @param instance     The instance to prepare.
 * @param sampleRate   The host's sample rate, in Hz.
 * @param maxBlockSize The largest block size PAX_process() will ever be
 *                      called with.
 */
void PAX_prepare (PAX_Instance* instance,
                  double        sampleRate,
                  int           maxBlockSize);

/**
 * @brief Process one block.
 *
 * Called on the AUDIO thread. **No heap allocation. No mutexes. No
 * blocking calls.**
 *
 * All per-block data is in ctx. Check ctx fields for NULL before use.
 * Value fields (valuesIn/valuesOut) are live — guard with:
 * @code
 * if (ctx->valuesOut && ctx->valueMaxCount > 0) { ... }
 * @endcode
 * @param instance The instance to process.
 * @param ctx      All per-block data — see PAX_ProcessContext.
 */
void PAX_process (PAX_Instance*           instance,
                  const PAX_ProcessContext* ctx);
/// @}

/**
 * @name Optional parameter exports
 * Stub with 0 / no-op if not needed.
 */
/// @{

/**
 * @brief Return the number of exposed parameters.
 * @param instance The instance to query.
 * @return Parameter count, 0 = none.
 */
int  PAX_getParameterCount (PAX_Instance* instance);

/**
 * @brief Fill *info with the details of the parameter at index.
 * @param instance The instance to query.
 * @param index    Which parameter (0-based).
 * @param info     Filled in with the parameter's name/range/default/step.
 */
void PAX_getParameterInfo  (PAX_Instance*      instance,
                             int                index,
                             PAX_ParameterInfo* info);

/**
 * @brief Get a parameter's current value (in its declared min/max range).
 * @param instance The instance to query.
 * @param index    Which parameter (0-based).
 * @return The parameter's current value.
 */
float PAX_getParameter (PAX_Instance* instance, int index);

/**
 * @brief Set a parameter's value (in its declared min/max range).
 * @param instance The instance to update.
 * @param index    Which parameter (0-based).
 * @param value    The new value.
 */
void  PAX_setParameter (PAX_Instance* instance, int index, float value);
/// @}

/**
 * @name Optional capability exports
 * The host checks for these symbols at load time and calls them only if
 * present. Pax that don't need them simply don't export them.
 */
/// @{

/**
 * @brief Return current audio output port count (for dynamic port Pax).
 *
 * Called by host after PAX_setParameter(index=0) if exported.
 * @param instance The instance to query.
 * @return Current audio output port count.
 */
int PAX_getAudioOutputCount (PAX_Instance* instance);

/**
 * @brief Return the number of Value input/output ports this Pax has.
 *
 * For nodeType 4, or any Pax that wants Value ports alongside audio/MIDI.
 * Static — no instance needed, called once at scan time, before any
 * instance exists. Not exporting these means 0 Value ports (safe default
 * for existing Pax binaries that predate Value port support). Fixed at
 * scan time, unlike PAX_getAudioOutputCount which can change per-instance
 * after a parameter changes — Value port counts for a given Pax don't.
 * @return Number of Value ports in that direction.
 */
int PAX_getValueInputCount  (void);
/// @copydoc PAX_getValueInputCount
int PAX_getValueOutputCount (void);

/**
 * @name Value port type tags
 *
 * What a specific Value port actually carries. Deliberately separate
 * integer constants, not the internal C++ PortType enum — keeps the
 * public Pax ABI decoupled from an internal enum whose ordering could
 * change later. The host translates these to its own PortType via a
 * small switch.
 *
 * MIDI is included here even though it also has its own legacy
 * midiInputs/midiOutputs fields on PAX_Descriptor — those keep working
 * unchanged for existing Pax, but PAX_VALUETYPE_MIDI lets a *new* Pax
 * mix a MIDI-typed port with other protocol types on ports declared
 * through this newer, more general per-port mechanism (e.g. one MIDI
 * input feeding differently-typed outputs on the same node). MIDI is
 * event-shaped identically to Value (PAX_MidiEvent and PAX_Value are
 * both array+count+maxCount) — Audio is not (continuous float buffers,
 * no event count), which is why Audio has no equivalent here and never
 * will without a much bigger change to the whole audio pipeline.
 */
/// @{
#define PAX_VALUETYPE_GENERIC 0
#define PAX_VALUETYPE_MQTT    1
#define PAX_VALUETYPE_OSC     2
#define PAX_VALUETYPE_DMX     3
#define PAX_VALUETYPE_UDP     4
#define PAX_VALUETYPE_ARTNET  5
#define PAX_VALUETYPE_MIDI    6
/// @}

/**
 * @brief Return the type tag (PAX_VALUETYPE_*) of the Value port at portIndex.
 *
 * portIndex is 0-based, in the same order implied by
 * PAX_getValueInputCount/OutputCount. Static, same reasoning as the count
 * functions above. Not exporting these, or returning an
 * out-of-range/unrecognised value, means every Value port defaults to
 * PAX_VALUETYPE_GENERIC — safe default for existing Pax binaries that
 * predate per-port typing.
 * @param portIndex Which Value port (0-based).
 * @return One of the PAX_VALUETYPE_* tags.
 */
int PAX_getValueInputType  (int portIndex);
/// @copydoc PAX_getValueInputType
int PAX_getValueOutputType (int portIndex);

/**
 * @name Colour category override
 *
 * See Architecture.md's "Hybrid vs Converter auto-detection" note for the
 * full rule. By default (not exporting this) the host auto-detects:
 * compares the set of port types on the input side against the output
 * side. A type present on only one side is being converted
 * (Converter/fuchsia); if every type mirrors on both sides, one distinct
 * type gets its own native colour, more than one gets Hybrid/orange. This
 * override is ONLY consulted in the one case the rule genuinely can't
 * resolve — a pure source or pure sink Pax, nothing to compare input
 * against output — so an author can pick a colour category explicitly
 * rather than have the host guess. Not consulted otherwise; the
 * auto-detected rule always wins when it can produce an answer, so this
 * can't make an already-clear node's colour misrepresent what it
 * actually does.
 */
/// @{
#define PAX_COLOURCAT_AUTO      -1  ///< Let the host auto-detect (default)
#define PAX_COLOURCAT_MIDI       0
#define PAX_COLOURCAT_AUDIO      1
#define PAX_COLOURCAT_HYBRID     2
#define PAX_COLOURCAT_CONVERTER  3
#define PAX_COLOURCAT_OSC        4
#define PAX_COLOURCAT_DMX        5
#define PAX_COLOURCAT_MQTT       6
#define PAX_COLOURCAT_UDP        7
#define PAX_COLOURCAT_ARTNET     8
/**
 * @brief Override the host's auto-detected colour category.
 * @return One of the PAX_COLOURCAT_* tags.
 */
int PAX_getColourCategory (void);
/// @}

/**
 * @brief Return FFT magnitude bin count (for spectrum display Pax).
 * @param instance The instance to query.
 * @return Number of magnitude bins.
 */
int PAX_getFFTSize (PAX_Instance* instance);

/**
 * @brief Return pointer to FFT magnitude array (for spectrum display Pax).
 * @param instance The instance to query.
 * @return Pointer to an array of PAX_getFFTSize() magnitude values.
 */
const float* PAX_getFFTMagnitudes (PAX_Instance* instance);
/// @}

#ifdef __cplusplus
} // extern "C"
#endif
