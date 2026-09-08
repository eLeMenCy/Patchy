import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, AudioPlayerFileLoaded, AudioPlayerStatus } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHandle, useNodeCollapsed, SettingsPanelHeader } from './NodeUtils';
import { Play, Pause, SkipBack, FolderOpen } from 'lucide-react';
import { HintContext } from './HintPanel';
import { NodeSelect } from './NodeSelect';

// ── Settings ──────────────────────────────────────────────────────────────────
// Field names/values match AudioPlayerNode.h's own restoreAudioPlayerSettings()
// parsing exactly (mode: 'file'/'sine'/'noise', noiseType: 'white'/'pink') —
// this is the one settings object whose own shape genuinely drives real-time
// backend audio processing, not just frontend display (see AudioPlayerNode.h's
// own comment on how this differs from AudioMonitorNode's settings).
export interface AudioPlayerSettings {
  customName: string;
  mode: 'file' | 'sine' | 'noise';
  sineFrequency: number;
  noiseType: 'white' | 'pink';
  level: number;      // 0.0 to 1.0
  loop: boolean;
  fileName: string;   // display only
  filePath: string;   // remembered so the backend can reload the actual audio
                       // data after a full app restart (see AudioPlayerState's
                       // own declaration comment — the loaded file itself only
                       // survives within a running session otherwise)
}

const DEFAULT_SETTINGS: AudioPlayerSettings = {
  customName: '',
  mode: 'sine',
  sineFrequency: 440,
  noiseType: 'white',
  level: 0.1,   // deliberately quiet by default — this can generate raw sine/noise
                 // tones, which could be startling or genuinely harmful at high
                 // volume, especially over headphones, before the user knows
                 // what to expect from a freshly-dropped or just-reset node
  loop: true,
  fileName: '',
  filePath: '',
};

export interface AudioPlayerNodeData {
  label:    string;
  nodeType: 26;
  ports:    { id: string; label: string; type: 'audio'; direction: 'output' }[];
  settings?: Partial<AudioPlayerSettings>;
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Colours ───────────────────────────────────────────────────────────────────
const COL_WAVE     = '#4FC3F7';   // same light blue as AudioMonitorNode's own left-channel colour
const COL_PLAYHEAD = '#FFC800';
const COL_GENERATOR_LINE = 'rgba(255,255,255,0.18)';
const COL_GENERATOR_LABEL = 'rgba(255,255,255,0.42)';
const BG = 'rgba(13, 15, 20, 0.52)';   // matches --bg with transparency, same as AudioMonitorNode

// ── Frequency slider log-scale mapping ──────────────────────────────────────────
// A linear 20Hz-20000Hz slider spends most of its own travel on frequencies far
// above what's musically/audibly distinguishable, while compressing the far more
// sensitive low end into a sliver — human pitch perception is logarithmic (each
// octave feels like an equal step), so the slider itself operates on a
// normalized 0-1 position, mapped exponentially to the actual frequency — the
// standard approach throughout audio software for exactly this reason. Verified
// against the geometric-mean midpoint (sqrt(20*20000) ≈ 632Hz at position 0.5)
// before use.
const FREQ_MIN = 20;
const FREQ_MAX = 20000;
function freqToSliderPos (freq: number): number {
  return Math.log (freq / FREQ_MIN) / Math.log (FREQ_MAX / FREQ_MIN);
}
function sliderPosToFreq (pos: number): number {
  return FREQ_MIN * Math.pow (FREQ_MAX / FREQ_MIN, pos);
}

// ── Level slider dB mapping ──────────────────────────────────────────────────
// The backend continues storing/using level as plain linear gain (0.0-1.0,
// unity at 1.0 — see AudioPlayerNode.h's own process() code, which multiplies
// samples by it directly) — only the slider's own display/interaction changes
// to dB. -60dB is treated as the slider's own practical minimum; at or below
// it, the actual stored level is true silence (0.0 exactly), not just a very
// quiet 10^(-60/20) ≈ 0.001 — matching how a fader's own bottom position
// works in most audio software.
const LEVEL_MIN_DB = -60;
function levelToDb (linear: number): number {
  if (linear <= 0) return LEVEL_MIN_DB;
  return Math.max (LEVEL_MIN_DB, 20 * Math.log10 (linear));
}
function dbToLevel (db: number): number {
  if (db <= LEVEL_MIN_DB) return 0;
  return Math.pow (10, db / 20);
}
function formatDb (db: number): string {
  return db <= LEVEL_MIN_DB ? '-\u221E' : db.toFixed (1);
}

// ── Drawing ───────────────────────────────────────────────────────────────────

// Static waveform (drawn from a fixed-size peaks summary, not live streaming
// samples — see AudioPlayerState::computeWaveformPeaks()'s own comment for
// why this is a one-time, whole-file summary rather than a scrolling view)
// plus a moving playhead line.
function drawWaveform (
  ctx: CanvasRenderingContext2D, w: number, h: number,
  peaks: number[], playFraction: number, playing: boolean
) {
  ctx.clearRect (0, 0, w, h);
  ctx.fillStyle = BG;
  ctx.fillRect (0, 0, w, h);

  const mid = h / 2;

  if (peaks.length >= 2) {
    const numBuckets = Math.floor (peaks.length / 2);
    const bucketW = w / numBuckets;
    ctx.fillStyle = COL_WAVE;
    for (let i = 0; i < numBuckets; i++) {
      const mn = peaks[i * 2];
      const mx = peaks[i * 2 + 1];
      const y1 = mid - mx * mid;
      const y2 = mid - mn * mid;
      ctx.fillRect (i * bucketW, y1, Math.max (1, bucketW), Math.max (1, y2 - y1));
    }
  } else {
    // No file loaded yet — flat centre line, so the waveform area doesn't
    // look broken/empty before a first load.
    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo (0, mid);
    ctx.lineTo (w, mid);
    ctx.stroke();
  }

  // Playhead
  const x = Math.max (0, Math.min (w, playFraction * w));
  ctx.strokeStyle = COL_PLAYHEAD;
  ctx.lineWidth = playing ? 2 : 1;
  ctx.beginPath();
  ctx.moveTo (x, 0);
  ctx.lineTo (x, h);
  ctx.stroke();
}

// Dimmed, static representative shape for Sine/Noise mode — deliberately not
// a live, continuously-updating preview (there's no meaningful "whole
// signal" to show for an infinite generated tone), just a visual cue plus a
// label naming what's actually playing.
function drawGeneratorPreview (
  ctx: CanvasRenderingContext2D, w: number, h: number,
  mode: 'sine' | 'noise', sineFrequency: number, noiseType: 'white' | 'pink'
) {
  ctx.clearRect (0, 0, w, h);
  ctx.fillStyle = BG;
  ctx.fillRect (0, 0, w, h);

  const mid = h / 2;
  ctx.strokeStyle = COL_GENERATOR_LINE;
  ctx.lineWidth = 1.5;
  ctx.beginPath();

  if (mode === 'sine') {
    for (let x = 0; x < w; x++) {
      const y = mid - Math.sin ((x / w) * Math.PI * 6) * (mid * 0.55);
      if (x === 0) ctx.moveTo (x, y); else ctx.lineTo (x, y);
    }
  } else {
    // A fixed, deterministic "noisy-looking" jagged line (a common
    // pseudo-random hash trick, not Math.random()) — a static shape that
    // doesn't flicker distractingly every animation frame, since this is
    // just a visual cue, not real noise data.
    for (let x = 0; x < w; x += 3) {
      const h1 = Math.sin (x * 12.9898) * 43758.5453;
      const rnd = h1 - Math.floor (h1);
      const y = mid + (rnd - 0.5) * mid * 1.1;
      if (x === 0) ctx.moveTo (x, y); else ctx.lineTo (x, y);
    }
  }
  ctx.stroke();

  ctx.fillStyle = COL_GENERATOR_LABEL;
  ctx.font = 'bold 18px "JetBrains Mono", monospace';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  const label = mode === 'sine'
    ? `Sine ${Math.round (sineFrequency)}Hz`
    : (noiseType === 'white' ? 'White' : 'Pink');
  ctx.fillText (label, w / 2, mid);
}

// ── Main component ───────────────────────────────────────────────────────────
function AudioPlayerNode ({ id, data, selected }: NodeProps) {
  const d = data as AudioPlayerNodeData;

  const [settings, setSettings] = useState<AudioPlayerSettings>({
    ...DEFAULT_SETTINGS, ...(d.settings ?? {}),
    ...(d.settingsJson ? JSON.parse (d.settingsJson) : {}),
  });
  const { showSettings, closeSettings, toggleSettings } = useNodeSettings (id);
  const { handleDelete } = useNodeDelete (id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, (data as any)._forceCollapsed);
  const { setHint } = useContext (HintContext);
  const portBodyRef = useRef<HTMLDivElement> (null);

  const canvasRef    = useRef<HTMLCanvasElement> (null);
  const settingsRef  = useRef (settings);
  const peaksRef      = useRef<number[]> ([]);
  const playFractionRef = useRef (0);
  const [playing, setPlaying] = useState (false);
  const rafRef        = useRef<number> (0);

  useEffect (() => { settingsRef.current = settings; }, [settings]);

  // Dispatches live updates for the fields that genuinely drive real-time
  // audio processing — bypasses the settings-commit/rebuild-only path
  // entirely (see PatchyProcessor::setAudioPlayerLiveParam's own comment
  // for why a settings commit alone was never enough on its own: it only
  // ever updates what gets SAVED, not what the currently-running audio
  // engine is actually doing). customName/fileName/filePath don't need
  // this at all — they're purely display/persistence, never read by the
  // audio thread.
  const dispatchLiveParams = useCallback ((p: Partial<AudioPlayerSettings>) => {
    if ('mode' in p) Bridge.setNodeParam (id, 'audioPlayerMode', p.mode ?? 'sine');
    if ('sineFrequency' in p) Bridge.setNodeParam (id, 'audioPlayerSineFrequency', String (p.sineFrequency ?? 440));
    if ('noiseType' in p) Bridge.setNodeParam (id, 'audioPlayerNoiseType', p.noiseType ?? 'white');
    if ('level' in p) Bridge.setNodeParam (id, 'audioPlayerLevel', String (p.level ?? 0.8));
    if ('loop' in p) Bridge.setNodeParam (id, 'audioPlayerLoop', p.loop ? '1' : '0');
  }, [id]);

  const patch = useCallback ((p: Partial<AudioPlayerSettings>) => {
    setSettings (s => {
      const next = { ...s, ...p };
      Bridge.setNodeSettings (id, next);
      return next;
    });
    dispatchLiveParams (p);
    if ('customName' in p)
      Bridge.setNodeLabel (id, p.customName ?? '');
  }, [id, dispatchLiveParams]);

  const commitPatch = useCallback ((p: Partial<AudioPlayerSettings>) => {
    setSettings (s => {
      const next = { ...s, ...p };
      Bridge.commitSettingsChange (id, next);
      return next;
    });
    dispatchLiveParams (p);
    if ('customName' in p)
      Bridge.setNodeLabel (id, p.customName ?? '');
  }, [id, dispatchLiveParams]);

  // Push the initial settings once on mount — a freshly-dropped node has
  // no settingsJson yet, so without this, the backend's own AudioPlayerState
  // would sit at its own struct defaults (file mode, no buffer — silence)
  // until the user happened to touch a control, even though the frontend
  // already shows a mode (sine, by default) as if it were live. Harmless
  // to also run this after a real project reload, since it just re-sends
  // the same values the backend's own restoreAudioPlayerSettings() already
  // applied via a different, equally valid path.
  useEffect (() => {
    dispatchLiveParams (settingsRef.current);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Request the current file info directly on mount, if a file should
  // already be loaded (per settings) but no peaks have arrived yet — a
  // real, confirmed gap otherwise: the backend's own one-shot push after a
  // restore-triggered load (project reload) fires on the next timer tick,
  // which can happen before this component has even mounted to listen for
  // it, silently missing the event with no retry. A direct pull here
  // avoids that race entirely.
  useEffect (() => {
    const s = settingsRef.current;
    if (s.mode === 'file' && s.fileName && peaksRef.current.length === 0)
      Bridge.audioPlayerRequestFileInfo (id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Restore settings from settingsJson on undo/redo
  useEffect (() => {
    const sj = (data as any)?.settingsJson;
    try {
      setSettings (s => ({ ...DEFAULT_SETTINGS, ...(sj ? JSON.parse (sj) : {}) }));
    } catch {}
  }, [(data as any)?.settingsJson]);

  // Subscribe to file-load results — updates the waveform peaks and the
  // file name/path shown in settings (and persisted via commitPatch, so it
  // survives both a rebuild and, via settingsJson, a full app restart).
  // Two distinct paths can fire this: a manual browse (always includes
  // filePath, since the frontend doesn't know it yet) and a restore-on-
  // rebuild load (omits filePath deliberately, since it already came FROM
  // the frontend's own known path) — only overwrite filePath when the
  // response actually includes one, so the restore path's own omission
  // never clobbers the existing, correct value with an empty string.
  useEffect (() => {
    const unsub = Bridge.onAudioPlayerFileLoaded ((result: AudioPlayerFileLoaded) => {
      if (result.nodeId !== id) return;
      if (result.success) {
        peaksRef.current = result.peaks ?? [];
        const patchFields: Partial<AudioPlayerSettings> = { fileName: result.fileName ?? '' };
        if (result.filePath) patchFields.filePath = result.filePath;
        commitPatch (patchFields);
      }
    });
    return () => { try { unsub(); } catch {} };
  }, [id, commitPatch]);

  // Subscribe to playback status — the only way the frontend can track a
  // continuously-advancing playhead, or notice a non-looping file having
  // reached its own end and auto-stopped on the backend side.
  useEffect (() => {
    const unsub = Bridge.onAudioPlayerStatus ((statuses: AudioPlayerStatus[]) => {
      const s = statuses.find (x => x.nodeId === id);
      if (!s) return;
      playFractionRef.current = s.fraction;
      setPlaying (s.playing);
    });
    return () => { try { unsub(); } catch {} };
  }, [id]);

  // Render loop
  useEffect (() => {
    const render = () => {
      const canvas = canvasRef.current;
      if (canvas) {
        const ctx = canvas.getContext ('2d');
        if (ctx) {
          if (settingsRef.current.mode === 'file') {
            drawWaveform (ctx, canvas.width, canvas.height,
                          peaksRef.current, playFractionRef.current, playing);
          } else {
            drawGeneratorPreview (ctx, canvas.width, canvas.height,
                                  settingsRef.current.mode as 'sine' | 'noise',
                                  settingsRef.current.sineFrequency,
                                  settingsRef.current.noiseType);
          }
        }
      }
      rafRef.current = requestAnimationFrame (render);
    };
    rafRef.current = requestAnimationFrame (render);
    return () => cancelAnimationFrame (rafRef.current);
  }, [collapsed, playing]);

  const handlePlayPause = useCallback (() => {
    Bridge.setNodeParam (id, 'audioPlayerPlaying', playing ? '0' : '1');
  }, [id, playing]);

  const handleReturnToStart = useCallback (() => {
    Bridge.setNodeParam (id, 'audioPlayerReturnToStart', '1');
  }, [id]);

  const handleSeek = useCallback ((e: React.MouseEvent<HTMLCanvasElement>) => {
    if (settingsRef.current.mode !== 'file') return;
    const rect = e.currentTarget.getBoundingClientRect();
    const fraction = (e.clientX - rect.left) / rect.width;
    Bridge.setNodeParam (id, 'audioPlayerSeek', String (Math.max (0, Math.min (1, fraction))));
  }, [id]);

  const handleLoadFile = useCallback (() => {
    Bridge.audioPlayerLoadFile (id);
  }, [id]);

  // Spacebar (play/pause) and double-space (return to start + stop) are
  // handled globally in App.tsx, scoped to whichever node is currently
  // selected — not here, since this component has no way to know about
  // global keyboard focus/selection state on its own.

  const W       = 320;
  const canvasH = 90;

  const transportBtnStyle: React.CSSProperties = {
    background: 'transparent',
    border: '1px solid var(--border)',
    borderRadius: 3,
    color: 'var(--text-muted)',
    cursor: 'pointer',
    width: 26,
    height: 22,
    padding: 0,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
  };

  return (
    <div style={{
      width: W,
      background: 'var(--surface)',
      border: `1px solid ${selected ? 'var(--audio)' : 'var(--border)'}`,
      borderTop: '3px solid var(--audio)',
      borderRadius: 'var(--radius)',
      boxShadow: selected
        ? '0 0 0 1px var(--audio), 0 8px 32px var(--audio-glow)'
        : '0 4px 16px rgba(0,0,0,.5)',
      position: 'relative',
      zIndex: showSettings ? 9999 : undefined,
    }}>

      {/* Audio Out only — this is a source, it has no audio input at all */}
      <NodeHandle nodeId={id} label="Audio Out" direction="out" colour="rgb(20,80,20)" index={0} total={1} offset={46} portBodyRef={portBodyRef} />

      {/* Header */}
      <NodeHeader title={settings.customName || "AUDIO PLAYER"} accent="var(--audio)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed} />

      {!collapsed && <>
      {/* Waveform/generator preview + level slider side by side */}
      <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}
           style={{ display:'flex', gap:4, padding:'6px 6px 4px' }}>

        <canvas
          ref={canvasRef}
          width={W - 44}
          height={canvasH}
          onClick={handleSeek}
          onMouseEnter={() => settings.mode === 'file' && setHint ({ title: 'Seek', body: 'Click anywhere on the waveform to jump playback there.' })}
          onMouseLeave={() => setHint (null)}
          style={{ borderRadius:3, display:'block', flexShrink:0,
                   cursor: settings.mode === 'file' ? 'pointer' : 'default' }}
        />

        {/* Level slider (vertical) — dB display, linear gain stored/used
            underneath (see the mapping helpers' own comment above) */}
        <div style={{ display:'flex', flexDirection:'column', alignItems:'center',
                      gap:2, width:28 }}>
          <span style={{ fontSize:8, color:'var(--text-muted)' }}>0dB</span>
          <input
            type="range" min={LEVEL_MIN_DB} max={0} step="any"
            onMouseEnter={() => setHint ({ title: 'Level', body: `${formatDb (levelToDb (settings.level))}dB. Double-click to reset.` })}
            onMouseLeave={() => setHint (null)}
            value={levelToDb (settings.level)}
            onChange={e => patch ({ level: dbToLevel (parseFloat (e.target.value)) })}
            onMouseUp={() => Bridge.commitNodeSettings (id)} onKeyUp={() => Bridge.commitNodeSettings (id)}
            onDoubleClick={e => { e.stopPropagation(); patch ({ level: DEFAULT_SETTINGS.level }); Bridge.commitNodeSettings (id); }}
            style={{ writingMode:'vertical-lr' as const, direction:'rtl' as const,
                     height: canvasH - 20, cursor:'pointer', flex:1 }}
          />
          <span style={{ fontSize:8, color:'var(--text-muted)' }}>{'-\u221E'}</span>
        </div>
      </div>

      {/* Transport buttons — Return to Start only makes sense in File mode
          (position is meaningless for a generated Sine/Noise signal); Stop
          was removed entirely as redundant with Pause, since both simply
          halt playback while keeping the current position */}
      <div className="nodrag" onMouseDown={e => e.stopPropagation()}
           style={{ display:'flex', justifyContent:'center', gap:8, padding:'2px 8px 8px' }}>
        {settings.mode === 'file' && (
          <button style={transportBtnStyle} onClick={handleReturnToStart}
            onMouseEnter={() => setHint ({ title: 'Return to Start', body: 'Jump playback back to the beginning.' })}
            onMouseLeave={() => setHint (null)}>
            <SkipBack size={13} />
          </button>
        )}
        <button style={{ ...transportBtnStyle, borderColor: playing ? 'var(--audio)' : 'var(--border)',
                          color: playing ? 'var(--audio)' : 'var(--text-muted)' }}
          onClick={handlePlayPause}
          onMouseEnter={() => setHint ({ title: playing ? 'Pause' : 'Play', body: playing ? 'Pause playback.' : 'Start playback.' })}
          onMouseLeave={() => setHint (null)}>
          {playing ? <Pause size={13} /> : <Play size={13} />}
        </button>
      </div>

      {showSettings && (
        <SettingsPanel s={settings} onChange={patch}
          onDiscreteChange={commitPatch} onClose={() => closeSettings()}
          onReset={() => commitPatch (DEFAULT_SETTINGS)} onCommit={() => Bridge.commitNodeSettings (id)}
          onLoadFile={handleLoadFile} />
      )}
      </>}
    </div>
  );
}

// ── Settings panel ───────────────────────────────────────────────────────────
function SettingsPanel ({ s, onChange, onDiscreteChange, onClose, onReset, onCommit, onLoadFile }: {
  s: AudioPlayerSettings;
  onChange: (p: Partial<AudioPlayerSettings>) => void;
  onDiscreteChange: (p: Partial<AudioPlayerSettings>) => void;
  onClose: () => void;
  onReset: () => void;
  onCommit: () => void;
  onLoadFile: () => void;
}) {
  const row = (label: string, child: React.ReactNode) => (
    <div style={{ display:'flex', alignItems:'center', gap:8, marginBottom:6 }}>
      <div style={{ width:90, fontSize:10, color:'var(--text-dim)', flexShrink:0 }}>{label}</div>
      <div style={{ flex:1, minWidth:0, display:'flex', alignItems:'center', overflow:'visible' }}>{child}</div>
    </div>
  );

  const section = (title: string) => (
    <div style={{ fontSize:9, color:'var(--text-dim)', textTransform:'uppercase',
                  letterSpacing:0.5, margin:'10px 0 4px', opacity:0.7 }}>{title}</div>
  );

  const { setHint: setSliderHint } = useContext (HintContext);

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position:'absolute', top:0, left:'100%', marginLeft:6,
        width:260, background:'var(--surface2)',
        border:'1px solid var(--border-hi)', borderRadius:'var(--radius)',
        padding:'10px 12px', zIndex:1000,
        boxShadow:'0 8px 32px rgba(0,0,0,.6)',
        fontFamily:"'JetBrains Mono', monospace",
        userSelect: 'none',
        overflow: 'visible',
      }}>
      <SettingsPanelHeader title="Audio Player" onReset={onReset} onClose={onClose} />

      {section ('Display')}
      {row ('Name', (
        <input type="text" value={s.customName} placeholder="Audio Player"
          onChange={e => onChange ({ customName: e.target.value })}
          onBlur={onCommit}
          style={{ width:'100%', background:'var(--surface)', border:'1px solid var(--border)',
                   borderRadius:3, color:'var(--text)', fontSize:10, padding:'3px 6px' }} />
      ))}

      {section ('Source')}
      {row ('Mode', (
        <NodeSelect
          value={s.mode}
          onChange={v => onDiscreteChange ({ mode: v as AudioPlayerSettings['mode'] })}
          options={[
            { id: 'file',  name: 'File' },
            { id: 'sine',  name: 'Sine' },
            { id: 'noise', name: 'Noise' },
          ]}
          accent="var(--audio)"
          showEmpty={false}
        />
      ))}

      {s.mode === 'file' && (
        <>
          {row ('File', (
            <button onClick={onLoadFile}
              style={{ display:'flex', alignItems:'center', gap:6, background:'var(--surface)',
                       border:'1px solid var(--border)', borderRadius:3, color:'var(--text)',
                       fontSize:10, padding:'4px 8px', cursor:'pointer', width:'100%' }}>
              <FolderOpen size={12} />
              {s.fileName || 'Browse…'}
            </button>
          ))}
          {row ('Loop', (
            <label style={{ display:'flex', alignItems:'center', gap:4, fontSize:10, color:'var(--text-dim)', cursor:'pointer' }}>
              <input type="checkbox" checked={s.loop}
                onChange={e => onDiscreteChange ({ loop: e.target.checked })} />
              Loop playback
            </label>
          ))}
        </>
      )}

      {s.mode === 'sine' && row ('Frequency', (
        <>
          <span style={{ fontSize:10, color:'var(--text-dim)', width:44, textAlign:'right',
                         flexShrink:0, fontVariantNumeric:'tabular-nums' }}>
            {Math.round (s.sineFrequency)}Hz
          </span>
          <input type="range" min={0} max={1} step="any"
            value={freqToSliderPos (s.sineFrequency)}
            onChange={e => onChange ({ sineFrequency: sliderPosToFreq (parseFloat (e.target.value)) })}
            onMouseUp={onCommit} onKeyUp={onCommit}
            onDoubleClick={e => { e.stopPropagation(); onChange ({ sineFrequency: DEFAULT_SETTINGS.sineFrequency }); onCommit(); }}
            onMouseEnter={() => setSliderHint ({ title: 'Frequency', body: 'Sine wave frequency, 20Hz to 20kHz. Double-click to reset.' })}
            onMouseLeave={() => setSliderHint (null)}
            style={{ flex:1, minWidth:0 }} />
        </>
      ))}

      {s.mode === 'noise' && row ('Colour', (
        <NodeSelect
          value={s.noiseType}
          onChange={v => onDiscreteChange ({ noiseType: v as AudioPlayerSettings['noiseType'] })}
          options={[
            { id: 'white', name: 'White' },
            { id: 'pink',  name: 'Pink' },
          ]}
          accent="var(--audio)"
          showEmpty={false}
        />
      ))}
    </div>
  );
}

export default memo (AudioPlayerNode);
