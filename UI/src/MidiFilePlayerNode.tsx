// MidiFilePlayerNode.tsx — v0.0.954 (user's idea, design agreed 2026-10-25)
//
// Plays a standard MIDI file (.mid) into the graph. One MIDI Out.
//   • Tempo (3-state button): FILE (the file's own tempo map, default) →
//     HOST (the DAW's tempo AND transport — plugin only; the Standalone
//     plays at the file's tempo) → MANUAL (own BPM, value box).
//   • Tracks: all play together, each event on its own channel; the ⚙ panel
//     lists them (name, channels, notes) with a mute each, and a Force channel.
//   • Transport: ⏮ back to start, ▶ / ❚❚, loop; click the bar to seek.
//     Pause / seek / mute send Note Offs for every sounding note.
// Everything that plays is live (midiFilePlayer* keys, no undo step); the
// settings (file, tempo mode, BPM, loop, force channel, muted tracks) are
// kept in settingsJson and restored by the backend on each rebuild.
import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, MidiFilePlayerStatus, MidiFilePlayerTrack } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeDisabled, NodeHeader, NodeHandle, useNodeCollapsed, SettingsPanelHeader, readoutBoxStyle } from './NodeUtils';
import ValueBox from './ValueBox';
import { Play, Pause, SkipBack, FolderOpen, Repeat } from 'lucide-react';
import { HintContext } from './HintPanel';

export type MidiTempoMode = 'file' | 'host' | 'manual';

export interface MidiFilePlayerSettings {
  customName:   string;
  fileName:     string;   // display
  filePath:     string;   // reloaded by the backend (project reopened, app restart)
  tempoMode:    MidiTempoMode;
  manualBpm:    number;
  loop:         boolean;
  forceChannel: number;   // 0 = off (each event keeps its own channel), 1-16
  muted:        number[]; // track indices
}

export const MIDI_PLAYER_DEFAULTS: MidiFilePlayerSettings = {
  customName: '', fileName: '', filePath: '',
  tempoMode: 'file', manualBpm: 120, loop: false, forceChannel: 0, muted: [],
};

export interface MidiFilePlayerNodeData {
  label:    string;
  nodeType: 29;
  ports:    { id: string; label: string; type: 'midi'; direction: 'output' }[];
  settingsJson?: string;
  [key: string]: unknown;
}

const ACCENT = 'var(--midi)';
const NEXT_MODE: Record<MidiTempoMode, MidiTempoMode> = { file: 'host', host: 'manual', manual: 'file' };
const MODE_LABEL: Record<MidiTempoMode, string> = { file: 'FILE', host: 'HOST', manual: 'MANUAL' };

const parseSettings = (sj?: string): MidiFilePlayerSettings => {
  try { return { ...MIDI_PLAYER_DEFAULTS, ...(sj ? JSON.parse (sj) : {}) }; }
  catch { return { ...MIDI_PLAYER_DEFAULTS }; }
};
const fmtTime = (s: number) => {
  if (! isFinite (s) || s < 0) s = 0;
  const m = Math.floor (s / 60), r = Math.floor (s % 60);
  return `${m}:${String (r).padStart (2, '0')}`;
};

/** Live keys for the backend's MidiFilePlayerState (no rebuild, no undo step). */
export function dispatchMidiPlayerLive (id: string, p: Partial<MidiFilePlayerSettings>) {
  if ('tempoMode'    in p) Bridge.setNodeParam (id, 'midiFilePlayerTempoMode',    p.tempoMode ?? 'file');
  if ('manualBpm'    in p) Bridge.setNodeParam (id, 'midiFilePlayerManualBpm',    String (p.manualBpm ?? 120));
  if ('loop'         in p) Bridge.setNodeParam (id, 'midiFilePlayerLoop',         p.loop ? '1' : '0');
  if ('forceChannel' in p) Bridge.setNodeParam (id, 'midiFilePlayerForceChannel', String (p.forceChannel ?? 0));
  if ('muted'        in p) Bridge.setNodeParam (id, 'midiFilePlayerMuted',        JSON.stringify (p.muted ?? []));
}

function MidiFilePlayerNode ({ id, data, selected }: NodeProps) {
  const d = data as MidiFilePlayerNodeData;
  const [settings, setSettings] = useState<MidiFilePlayerSettings> (() => parseSettings (d.settingsJson));
  const settingsRef = useRef (settings);
  settingsRef.current = settings;

  const { showSettings, closeSettings, toggleSettings } = useNodeSettings (id);
  const { handleDelete } = useNodeDelete (id);
  const { disabled, toggleDisabled } = useNodeDisabled (id, (data as any).disabled);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, (data as any)._forceCollapsed);
  const { setHint } = useContext (HintContext);
  const portBodyRef = useRef<HTMLDivElement> (null);

  // Live status (30 Hz) and the loaded file's info
  const [fraction, setFraction] = useState (0);
  const [playing,  setPlaying]  = useState (false);
  const [bpm,      setBpm]      = useState (120);
  const [host,     setHost]     = useState (false);
  const [tracks,   setTracks]   = useState<MidiFilePlayerTrack[]> ([]);
  const [fileBpm,  setFileBpm]  = useState (120);
  const [seconds,  setSeconds]  = useState (0);

  const commitPatch = useCallback ((p: Partial<MidiFilePlayerSettings>) => {
    const next = { ...settingsRef.current, ...p };
    settingsRef.current = next;
    setSettings (next);
    Bridge.commitSettingsChange (id, next);
    dispatchMidiPlayerLive (id, p);
    if ('customName' in p) Bridge.setNodeLabel (id, p.customName ?? '');
  }, [id]);

  // Mount: the backend state gets this node's settings (a freshly-dropped
  // node has none yet) and sends its file info back (it may have loaded the
  // file before this component listened)
  useEffect (() => {
    const s = settingsRef.current;
    dispatchMidiPlayerLive (id, { tempoMode: s.tempoMode, manualBpm: s.manualBpm, loop: s.loop, forceChannel: s.forceChannel, muted: s.muted });
    Bridge.setNodeParam (id, 'midiFilePlayerRequestInfo', '1');
  }, [id]);

  // Undo / redo
  useEffect (() => { setSettings (parseSettings (d.settingsJson)); }, [d.settingsJson]);

  useEffect (() => Bridge.onMidiFilePlayerStatus ((st: MidiFilePlayerStatus[]) => {
    const s = st.find (x => x.nodeId === id);
    if (! s) return;
    setFraction (s.fraction); setPlaying (s.playing); setBpm (s.bpm); setHost (s.host);
    if (s.file) {
      setTracks (s.file.tracks ?? []);
      setFileBpm (s.file.fileBpm ?? 120);
      setSeconds (s.file.seconds ?? 0);
      // A newly chosen file (or back to the demo): remember it (one undo
      // step); its tracks are new, so no mutes
      const path = s.file.demo ? '' : (s.file.filePath ?? '');
      if (path !== settingsRef.current.filePath && (path || s.file.demo))
        commitPatch ({ filePath: path, fileName: s.file.demo ? '' : s.file.fileName ?? '', muted: [] });
    }
  }), [id, commitPatch]);

  const hasFile    = true;   // v0.0.955 — no file set = the built-in demo, always playable
  const fileLabel  = settings.filePath ? settings.fileName : 'Catchy Patchy — melodic trance';
  const hostDriven = settings.tempoMode === 'host' && host;   // the DAW plays / stops it
  const togglePlay = () => Bridge.setNodeParam (id, 'midiFilePlayerPlaying', playing ? '0' : '1');
  const toStart    = () => Bridge.setNodeParam (id, 'midiFilePlayerReturnToStart', '1');
  const seek = (e: React.MouseEvent<HTMLDivElement>) => {
    if (! hasFile || hostDriven) return;
    const r = e.currentTarget.getBoundingClientRect();
    Bridge.setNodeParam (id, 'midiFilePlayerSeek', String (Math.max (0, Math.min (1, (e.clientX - r.left) / r.width))));
  };

  const tip = (title: string, body: string) => ({ onMouseEnter: () => setHint ({ title, body }), onMouseLeave: () => setHint (null) });
  const btn = (on: boolean, off = false): React.CSSProperties => ({
    background: on ? 'color-mix(in srgb, var(--midi) 20%, transparent)' : 'transparent',
    border: `1px solid ${on ? ACCENT : 'var(--border)'}`, borderRadius: 3,
    color: on ? ACCENT : 'var(--text-muted)', cursor: off ? 'default' : 'pointer',
    opacity: off ? 0.4 : 1, height: 22, minWidth: 26, padding: '0 4px',
    display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 9,
    fontFamily: "'JetBrains Mono', monospace",
  });

  const modeHint = settings.tempoMode === 'file'
    ? 'FILE: the file\'s own tempo (and tempo changes). Click for HOST.'
    : settings.tempoMode === 'host'
    ? (host ? 'HOST: follows the DAW — its tempo, play / stop and position. Click for MANUAL.'
            : 'HOST: follows the DAW in the plugin. Here (Standalone) it plays at the file\'s tempo with its own transport. Click for MANUAL.')
    : 'MANUAL: plays at the BPM set on the right (drag / double-click to type). Click for FILE.';

  const W = 260;
  return (
    <div style={{
      minWidth: W, background: 'var(--surface)',
      border: `1px solid ${selected ? ACCENT : 'var(--border)'}`, borderTop: `3px solid ${ACCENT}`,
      borderRadius: 'var(--radius)',
      boxShadow: selected ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--midi-glow)` : '0 4px 16px rgba(0,0,0,.5)',
      position: 'relative', zIndex: showSettings ? 9999 : undefined,
      filter: disabled ? 'grayscale(0.8) opacity(0.55)' : 'none', transition: 'filter .15s',
    }}>
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour={ACCENT} index={0} total={1} anchor="bar" portBodyRef={portBodyRef} />

      <NodeHeader title={settings.customName || 'MIDI FILE PLAYER'}
        subtitle={collapsed ? `${playing ? '▶' : '❚❚'} ${settings.filePath ? settings.fileName : 'Catchy Patchy'}` : undefined}
        rename={{ value: settings.customName ?? '', placeholder: 'MIDI File Player', onCommit: v => commitPatch ({ customName: v }) }}
        accent={ACCENT} showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
        disabled={disabled} onToggleDisabled={toggleDisabled} />

      {! collapsed && (
        <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()} onPointerDown={e => e.stopPropagation()}
             style={{ padding: '6px 8px 8px', display: 'flex', flexDirection: 'column', gap: 6 }}>
          {/* File */}
          <button onClick={() => Bridge.midiFilePlayerLoadFile (id)}
            {...tip ('MIDI file', settings.filePath ? `${settings.filePath}\nClick to choose another .mid file. (⚙: back to the demo)`
                                                    : 'No file set: the built-in demo plays — Catchy Patchy, 8 bars of melodic trance, 5 tracks. Click to choose a .mid file.')}
            style={{ display: 'flex', alignItems: 'center', gap: 6, background: 'var(--surface2)', border: '1px solid var(--border)',
                     borderRadius: 3, color: hasFile ? 'var(--text)' : 'var(--text-muted)', fontSize: 10, padding: '4px 8px',
                     cursor: 'pointer', width: '100%', minWidth: 0, fontFamily: "'JetBrains Mono', monospace" }}>
            <FolderOpen size={12} style={{ flexShrink: 0 }} />
            <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontStyle: settings.filePath ? 'normal' : 'italic' }}>{fileLabel}</span>
          </button>

          {/* Position (click to seek) */}
          <div data-port-anchor="bar" onClick={seek}
            {...tip ('Position', hostDriven ? 'Follows the DAW\'s position (HOST).' : 'Click to jump there.')}
            style={{ position: 'relative', height: 14, borderRadius: 3, background: 'rgba(13,15,20,0.52)',
                     border: '1px solid var(--border)', cursor: hasFile && ! hostDriven ? 'pointer' : 'default', overflow: 'hidden' }}>
            <div style={{ position: 'absolute', left: 0, top: 0, bottom: 0, width: `${fraction * 100}%`,
                          background: 'color-mix(in srgb, var(--midi) 35%, transparent)' }} />
            <div style={{ position: 'absolute', left: `${fraction * 100}%`, top: 0, bottom: 0, width: playing ? 2 : 1, background: '#FFC800' }} />
            <span style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
                           fontSize: 8, color: 'var(--text-dim)', fontFamily: "'JetBrains Mono', monospace", pointerEvents: 'none' }}>
              {hasFile ? `${fmtTime (fraction * seconds)} / ${fmtTime (seconds)}` : '—'}
            </span>
          </div>

          {/* Transport + tempo */}
          <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <button style={btn (false, hostDriven)} onClick={hostDriven ? undefined : toStart}
              {...tip ('Back to start', hostDriven ? 'The DAW drives the position (HOST).' : 'Jump back to the beginning.')}><SkipBack size={12} /></button>
            <button style={btn (playing, hostDriven)} onClick={hostDriven ? undefined : togglePlay}
              {...tip (playing ? 'Pause' : 'Play', hostDriven ? 'The DAW\'s play / stop drives it (HOST).' : playing ? 'Pause (sounding notes are released).' : 'Start playback.')}>
              {playing ? <Pause size={12} /> : <Play size={12} />}
            </button>
            <button style={btn (settings.loop)} onClick={() => commitPatch ({ loop: ! settings.loop })}
              {...tip ('Loop', settings.loop ? 'Loop on: back to the start at the end.' : 'Loop off: stops at the end and goes back to the start.')}><Repeat size={12} /></button>
            <div style={{ flex: 1 }} />
            <button style={{ ...btn (settings.tempoMode !== 'file'), minWidth: 52, opacity: settings.tempoMode === 'host' && ! host ? 0.6 : 1 }}
              onClick={() => commitPatch ({ tempoMode: NEXT_MODE[settings.tempoMode] })}
              {...tip (`Tempo: ${MODE_LABEL[settings.tempoMode]}`, modeHint)}>{MODE_LABEL[settings.tempoMode]}</button>
            {settings.tempoMode === 'manual'
              ? <ValueBox value={settings.manualBpm} min={20} max={300} step={1} fineStep={0.1} decimals={1} accent={ACCENT}
                  onCommit={v => commitPatch ({ manualBpm: v })}
                  onLive={v => Bridge.setNodeParam (id, 'midiFilePlayerManualBpm', String (v))}
                  title="BPM — drag up/down (⇧ fine) • double-click to type" width={44} />
              : <span {...tip ('BPM', settings.tempoMode === 'file' ? `The file's tempo now (starts at ${fileBpm.toFixed (1)}).` : 'The tempo it plays at now.')}
                  style={{ ...readoutBoxStyle ('var(--text-dim)'), width: 44, textAlign: 'center', fontSize: 10, padding: '2px 4px', boxSizing: 'border-box' }}>
                  {(playing || settings.tempoMode === 'host' ? bpm : fileBpm).toFixed (1)}
                </span>}
          </div>
        </div>
      )}

      {showSettings && ! collapsed && (
        <SettingsPanel s={settings} tracks={tracks} onChange={commitPatch} onClose={closeSettings}
          onUseDemo={() => { commitPatch ({ filePath: '', fileName: '', muted: [] }); Bridge.setNodeParam (id, 'midiFilePlayerUseDemo', '1'); }}
          onReset={() => commitPatch ({ tempoMode: 'file', manualBpm: 120, loop: false, forceChannel: 0, muted: [] })} />
      )}
    </div>
  );
}

function SettingsPanel ({ s, tracks, onChange, onClose, onReset, onUseDemo }: {
  s: MidiFilePlayerSettings; tracks: MidiFilePlayerTrack[];
  onChange: (p: Partial<MidiFilePlayerSettings>) => void; onClose: () => void; onReset: () => void; onUseDemo: () => void;
}) {
  const { setHint } = useContext (HintContext);
  const tip = (title: string, body: string) => ({ onMouseEnter: () => setHint ({ title, body }), onMouseLeave: () => setHint (null) });
  const muted = new Set (s.muted);
  const toggleMute = (i: number) => {
    const next = new Set (muted); if (next.has (i)) next.delete (i); else next.add (i);
    onChange ({ muted: [...next].sort ((a, b) => a - b) });
  };
  const section = (t: string) => (
    <div style={{ fontSize: 9, color: 'var(--text-dim)', textTransform: 'uppercase', letterSpacing: 0.5, margin: '10px 0 4px', opacity: 0.7 }}>{t}</div>
  );
  const chans = (t: MidiFilePlayerTrack) =>
    t.channels.length === 0 ? '—' : t.channels.length > 4 ? `ch ${t.channels.slice (0, 4).join (',')}…` : `ch ${t.channels.join (',')}`;
  const playable = tracks.map ((t, i) => ({ t, i })).filter (x => x.t.notes > 0 || x.t.channels.length > 0);

  return (
    <div className="nodrag"
      onMouseDown={e => e.stopPropagation()} onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()} onPointerUp={e => e.stopPropagation()} onClick={e => e.stopPropagation()}
      style={{ position: 'absolute', top: 0, left: '100%', marginLeft: 6, width: 270, background: 'var(--surface2)',
               border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)', padding: '10px 12px', zIndex: 1000,
               boxShadow: '0 8px 32px rgba(0,0,0,.6)', fontFamily: "'JetBrains Mono', monospace", userSelect: 'none' }}>
      <SettingsPanelHeader title="MIDI File Player" onReset={onReset} onClose={onClose} />

      {section ('Source')}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <div style={{ flex: 1, minWidth: 0, fontSize: 10, color: s.filePath ? 'var(--text)' : 'var(--text-dim)',
                      overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontStyle: s.filePath ? 'normal' : 'italic' }}>
          {s.filePath ? s.fileName : 'Catchy Patchy — melodic trance (138 BPM)'}
        </div>
        {s.filePath && (
          <button onClick={onUseDemo}
            {...tip ('Back to the demo', 'Forget the file and play the built-in demo (Catchy Patchy) again. Undoable (⌘Z).')}
            style={{ fontSize: 9, padding: '2px 8px', borderRadius: 3, cursor: 'pointer', background: 'transparent',
                     border: '1px solid var(--border)', color: 'var(--text-muted)', fontFamily: "'JetBrains Mono', monospace" }}>
            Demo
          </button>
        )}
      </div>

      {section ('Output')}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}
        {...tip ('Force channel', 'Off: every event keeps the channel it has in the file. 1-16: everything goes out on that channel.')}>
        <div style={{ width: 90, fontSize: 10, color: 'var(--text-dim)' }}>Force channel</div>
        <ValueBox value={s.forceChannel} min={0} max={16} minWord="Off" accent={ACCENT}
          onCommit={v => onChange ({ forceChannel: v })} width={44} />
      </div>

      {section (`Tracks${tracks.length ? ` (${playable.length})` : ''}`)}
      {playable.length === 0
        ? <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>{tracks.length ? 'No notes or controllers in this file.' : '…'}</div>
        : <div style={{ display: 'flex', flexDirection: 'column', gap: 2, maxHeight: 220, overflowY: 'auto' }}>
            {playable.map (({ t, i }) => {
              const on = ! muted.has (i);
              return (
                <div key={i} onClick={() => toggleMute (i)}
                  {...tip (t.name, `${on ? 'Playing' : 'Muted'} — click to ${on ? 'mute' : 'unmute'}. ${t.notes} note(s), ${chans (t)}.`)}
                  style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 10, padding: '2px 4px', borderRadius: 3, cursor: 'pointer',
                           background: on ? 'transparent' : 'rgba(255,255,255,0.03)', opacity: on ? 1 : 0.45 }}>
                  <span style={{ width: 10, color: on ? ACCENT : 'var(--text-muted)' }}>{on ? '●' : '○'}</span>
                  <span style={{ flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                                 color: 'var(--text)', textDecoration: on ? 'none' : 'line-through' }}>{t.name}</span>
                  <span style={{ fontSize: 9, color: 'var(--text-dim)', whiteSpace: 'nowrap' }}>{chans (t)}</span>
                  <span style={{ fontSize: 9, color: 'var(--text-muted)', width: 34, textAlign: 'right' }}>{t.notes}</span>
                </div>
              );
            })}
          </div>}
    </div>
  );
}

export default memo (MidiFilePlayerNode);
