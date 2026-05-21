/**
 * AddonAPI.h  —  Patchy Addon API v1
 *
 * This is the ONLY file a addon author needs.
 * No JUCE dependency. No NodeGraph source dependency.
 *
 * Build your addon as a shared library:
 *   macOS:   clang++ -std=c++17 -shared -fPIC MyNode.cpp -o MyNode.dylib
 *   Linux:   g++     -std=c++17 -shared -fPIC MyNode.cpp -o MyNode.so
 *   Windows: cl /std:c++17 /LD MyNode.cpp /Fe:MyNode.dll
 *
 * Drop the binary into the NodeGraph addons folder and restart.
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  API version — host rejects addons built against a different major version
// ─────────────────────────────────────────────────────────────────────────────
#define NGA_API_VERSION 1

// ─────────────────────────────────────────────────────────────────────────────
//  Opaque instance handle
//  Allocate whatever internal struct you need and cast its pointer to this.
// ─────────────────────────────────────────────────────────────────────────────
typedef void NGA_Instance;

// ─────────────────────────────────────────────────────────────────────────────
//  MIDI event
//
//  Messages are passed as an array of NGA_MidiEvent structs.
//  sampleOffset : sample position within the current block (0 … numSamples-1)
//  byteCount    : number of valid bytes (1–3)
//  bytes        : raw MIDI bytes  e.g. { 0x90, 60, 100 } = note-on C4 vel 100
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    int32_t sampleOffset;
    uint8_t byteCount;
    uint8_t bytes[3];
} NGA_MidiEvent;

// ─────────────────────────────────────────────────────────────────────────────
//  Descriptor  (static metadata — no instance required)
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    const char* name;          // Display name  e.g. "Gain",  "Transpose"
    const char* vendor;        // Author/studio e.g. "ACME Audio"
    const char* version;       // Semver string e.g. "1.0.0"
    int         nodeType;      // 1 = MIDI only
                               // 2 = Audio only
                               // 3 = MIDI + Audio (AV)
    int         apiVersion;    // Must equal NGA_API_VERSION

    // ── Optional port counts (0 = use nodeType defaults) ──────────────────
    // Set these to override the default 1-in / 1-out layout.
    // e.g. a stereo splitter: audioInputs=1, audioOutputs=2
    int         audioInputs;   // Number of audio input ports  (0 = default)
    int         audioOutputs;  // Number of audio output ports (0 = default)
    int         midiInputs;    // Number of MIDI input ports   (0 = default)
    int         midiOutputs;   // Number of MIDI output ports  (0 = default)
} NGA_Descriptor;

// ─────────────────────────────────────────────────────────────────────────────
//  Parameter info  (for future use — implement stubs returning 0 / nullptr)
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    const char* name;          // "Gain",  "Semitones" …
    float       minValue;
    float       maxValue;
    float       defaultValue;
    float       step;          // 0 = continuous, 1 = integer steps, etc.
} NGA_ParameterInfo;

// ─────────────────────────────────────────────────────────────────────────────
//  Required exports  (every addon MUST provide all of these)
// ─────────────────────────────────────────────────────────────────────────────

/** Return a pointer to a static NGA_Descriptor. Called once at scan time.
 *  Must never return NULL. */
const NGA_Descriptor* NGA_getDescriptor (void);

/** Create a new processing instance. Called on the message thread.
 *  Return NULL on failure. */
NGA_Instance* NGA_create (void);

/** Destroy an instance previously returned by NGA_create(). */
void NGA_destroy (NGA_Instance* instance);

/** Called before processing starts or when host settings change.
 *  Called on the MESSAGE thread — safe to allocate here.
 *  Will always be called before the first NGA_process(). */
void NGA_prepare (NGA_Instance* instance,
                  double        sampleRate,
                  int           maxBlockSize);

/** Process one block.  Called on the AUDIO thread.
 *  *** No heap allocation. No mutexes. No blocking calls. ***
 *
 *  audioIn / audioOut : arrays of [numChannels] float* pointers,
 *                       each pointing to [numSamples] samples.
 *                       NULL when nodeType == 1 (MIDI-only).
 *  numChannels        : always 2 (stereo).
 *  numSamples         : block size (≤ maxBlockSize passed to NGA_prepare).
 *  midiIn             : input MIDI events for this block (may be NULL).
 *  midiInCount        : number of events in midiIn (0 if none).
 *  midiOut            : write output MIDI events here.
 *  midiOutCount       : set *midiOutCount to the number of events written.
 *  midiMaxCount       : maximum events you may write to midiOut. */
void NGA_process (NGA_Instance*        instance,
                  float**              audioIn,
                  float**              audioOut,
                  int                  numChannels,
                  int                  numSamples,
                  const NGA_MidiEvent* midiIn,
                  int                  midiInCount,
                  NGA_MidiEvent*       midiOut,
                  int*                 midiOutCount,
                  int                  midiMaxCount);

// ─────────────────────────────────────────────────────────────────────────────
//  Optional parameter exports  (stub with 0 / no-op for now)
// ─────────────────────────────────────────────────────────────────────────────

/** Return number of exposed parameters (0 = none). */
int  NGA_getParameterCount (NGA_Instance* instance);

/** Fill *info for parameter at index. */
void NGA_getParameterInfo  (NGA_Instance*     instance,
                             int               index,
                             NGA_ParameterInfo* info);

/** Get / set a parameter value (normalised 0..1 or in declared min/max range). */
float NGA_getParameter (NGA_Instance* instance, int index);
void  NGA_setParameter (NGA_Instance* instance, int index, float value);

#ifdef __cplusplus
} // extern "C"
#endif
