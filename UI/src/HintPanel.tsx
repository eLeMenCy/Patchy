import { createContext, useContext, useState, ReactNode } from 'react';

// ── Hint context ──────────────────────────────────────────────────────────────
interface HintState { title: string; body: string; }
export const HintContext = createContext<{
  hint: HintState | null;
  setHint: (h: HintState | null) => void;
}>({ hint: null, setHint: () => {} });

export function HintProvider ({ children }: { children: ReactNode }) {
  const [hint, setHint] = useState<HintState | null>(null);
  return (
    <HintContext.Provider value={{ hint, setHint }}>
      {children}
    </HintContext.Provider>
  );
}


// ── Port hints ────────────────────────────────────────────────────────────────
export const PORT_HINTS: Record<string, { title: string; body: string }> = {
  'midi-in':   { title: 'MIDI Input',   body: 'Receives MIDI data — notes, CC, pitch bend, clock etc. Connect from a MIDI source.' },
  'midi-out':  { title: 'MIDI Output',  body: 'Sends MIDI data downstream. Connect to a device, monitor or another processor.' },
  'audio-in':  { title: 'Audio Input',  body: 'Receives an audio signal (stereo, 32-bit float). Connect from an audio source.' },
  'audio-out': { title: 'Audio Output', body: 'Sends an audio signal downstream. Connect to a device, monitor or another processor.' },
};

export const EDGE_HINTS: Record<string, { title: string; body: string }> = {
  'midi':  { title: 'MIDI Connection',  body: 'Carries MIDI data between nodes. Flashes on activity.' },
  'audio': { title: 'Audio Connection', body: 'Carries a stereo audio signal between nodes. Colour shows signal level.' },
  'av':    { title: 'Hybrid Connection',body: 'Carries both MIDI and audio signals between nodes.' },
  'value': { title: 'Value Connection', body: 'Carries a generic numeric value between nodes (a Pax adapter/converter port).' },
  'osc':   { title: 'OSC Connection',   body: 'Carries OSC (Open Sound Control) messages between nodes. Flashes on activity.' },
  'dmx':   { title: 'DMX Connection',   body: 'Carries DMX512 (or ArtNet) lighting data between nodes. Colour shows channel intensity.' },
  'mqtt':  { title: 'MQTT Connection',  body: 'Carries MQTT messages between nodes. Flashes on activity.' },
  'udp':   { title: 'UDP Connection',   body: 'Carries raw UDP datagrams between nodes. Flashes on activity.' },
};

// ── HintPanel — renders at bottom of sidebar ─────────────────────────────────
export function HintPanel () {
  const { hint } = useContext(HintContext);
  return (
    <div style={{
      borderTop:   '1px solid var(--border)',
      padding:     '10px 12px',
      minHeight:   80,
      transition:  'opacity 0.15s',
      opacity:     1,
    }}>
      {hint ? (
        <>
          <div style={{
            fontSize:      9,
            fontWeight:    700,
            letterSpacing: '0.1em',
            color:         'var(--accent)',
            marginBottom:  4,
            fontFamily:    "'Syne', sans-serif",
            textTransform: 'uppercase',
          }}>
            {hint.title}
          </div>
          <div style={{
            fontSize:    10,
            color:       'var(--text-dim)',
            lineHeight:  1.5,
            fontFamily:  "'JetBrains Mono', monospace",
            whiteSpace:  'pre-line',
          }}>
            {hint.body}
          </div>
        </>
      ) : (
        <div style={{
          fontSize:   10,
          color:      'var(--text-dim)',
          fontFamily: "'JetBrains Mono', monospace",
          lineHeight: 1.5,
        }}>
          Hover any node, button or<br/>parameter to see a hint here.
        </div>
      )}
    </div>
  );
}

// ── Node hint descriptions ────────────────────────────────────────────────────
export const NODE_HINTS: Record<string, { title: string; body: string }> = {
  // Built-in nodes
  'MIDI IN DEVICE':   { title: 'MIDI In Device',   body: 'Receives MIDI from an external device or virtual port. Select your controller or DAW output from the dropdown.' },
  'MIDI OUT DEVICE':  { title: 'MIDI Out Device',  body: 'Sends MIDI to an external device or virtual port. Select your synth, DAW input or other destination.' },
  'AUDIO IN DEVICE':  { title: 'Audio In Device',  body: 'Captures audio from a hardware input or system source. Select your interface or microphone.' },
  'AUDIO OUT DEVICE': { title: 'Audio Out Device', body: 'Sends audio to a hardware output or system destination. Select your speakers or interface.' },
  'MIDI MONITOR':     { title: 'MIDI Monitor',     body: 'Displays incoming MIDI events in real time. Filter by channel or event type. Double-click header to fold.' },
  'AUDIO MONITOR':    { title: 'Audio Monitor',    body: 'Oscilloscope view of the audio signal. Supports L/R overlay, separate channels, amplitude zoom and clip detection.' },
  'MIDI KEYBOARD':    { title: 'MIDI Keyboard',    body: 'On-screen MIDI keyboard. Click or drag to play notes. Supports pitch bend, mod wheel and configurable velocity.' },
  'UDP IN DEVICE':       { title: 'UDP In Device',       body: 'Listens for UDP datagrams on a configured port. Supports unicast, multicast and broadcast. Set the port in the node settings to activate.' },
  'UDP OUT DEVICE':      { title: 'UDP Out Device',      body: 'Sends incoming graph values as UDP datagrams to a configured target. Supports unicast (host:port), multicast and broadcast.' },
  'ARTNET IN DEVICE':   { title: 'ArtNet In Device',    body: 'Listens for Art-Net ArtDmx packets on UDP port 6454. Outputs the DMX universe blob as a PAX_Value. Configure the universe number to filter.' },
  'ARTNET OUT DEVICE':  { title: 'ArtNet Out Device',   body: 'Sends incoming DMX blobs as Art-Net ArtDmx packets to a configured target host on port 6454. Set universe and target host in settings.' },
  'DMX IN DEVICE':      { title: 'DMX In Device',       body: 'Receives DMX512 from an Enttec DMX USB Pro interface. Select your serial port in settings. Outputs a DMX universe blob as a PAX_Value.' },
  'DMX OUT DEVICE':     { title: 'DMX Out Device',      body: 'Sends DMX512 to an Enttec DMX USB Pro interface. Select your serial port in settings. Takes a DMX universe blob from the graph.' },
  'DMX MONITOR':        { title: 'DMX Monitor',         body: 'Displays all 512 DMX channel values in real time. Pass-through — incoming DMX flows unchanged to the output port. Page through channels with ◀ ▶.' },
  'DMX CONSOLE':        { title: 'DMX Console',         body: 'Control up to 512 DMX channels with interactive faders. Drag faders or click values to edit. BO = Blackout (all channels to 0). Upstream DMX overrides faders when connected.' },
  'OSC IN DEVICE':    { title: 'OSC In Device',    body: 'Listens for OSC 1.0 messages on a UDP port. Parses address pattern and typed arguments (f, i, s, b, T, F). Set the port in the node settings to activate.' },
  'OSC OUT DEVICE':   { title: 'OSC Out Device',   body: 'Sends incoming graph values as OSC messages to a configured host:port. The OSC address is configurable (default /patchy).' },
  // Pax
  'AudioLevel':  { title: 'Audio Level',    body: 'Controls signal level from -60dB (silence) to +6dB. Use before outputs or between processing nodes.' },
  'Level':       { title: 'Audio Level',    body: 'Controls signal level from -60dB (silence) to +6dB. Use before outputs or between processing nodes.' },
  'Amp':         { title: 'Amp',            body: 'Boosts the signal from 0dB (unity) to +24dB. Use to drive weak signals or compensate for low levels.' },
  'Transpose':   { title: 'Transpose',      body: 'Shifts MIDI pitch by -24 to +24 semitones. Useful for transposing keyboards or creating harmonies.' },
  'Envelope':    { title: 'Envelope',       body: 'Converts audio amplitude or a frequency band level into a MIDI CC stream. Ideal for driving LED controllers or DAW automation.' },
  'Splitter':    { title: 'Stereo Splitter',body: 'Splits a stereo signal into separate Left and Right mono outputs. Balance control adjusts the L/R ratio.' },
};

// ── Button hints ──────────────────────────────────────────────────────────────
export const BUTTON_HINTS = {
  foldAll:     { title: 'Fold / Unfold All',  body: 'Collapses all nodes to their header bar for a clean overview. Press again to expand all. Shortcut: F' },
  preferences: { title: 'Graph Preferences',  body: 'Open the preferences panel to configure graph behaviour and appearance.' },
  settings:    { title: 'Node Settings',      body: 'Open the settings panel for this node to configure its options.' },
  reset:       { title: 'Reset to Defaults',  body: 'Resets all settings or parameters to their default values.' },
  close:       { title: 'Close Panel',        body: 'Closes this settings panel.' },
  deleteNode:  { title: 'Delete Node',        body: 'Permanently removes this node and all its connections from the graph.' },
  fold:        { title: 'Fold / Unfold Node', body: 'Collapses or expands this node. Double-click the header to toggle. Shortcut: F (all nodes).' },
};
