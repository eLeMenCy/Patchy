import { useEffect, useRef, useState, useContext } from 'react';
import { Bridge, AudioSettings } from './Bridge';
import { DawContext } from './DawContext';
import { HintContext } from './HintPanel';

// PreferencesPanel.tsx — the standalone/DAW preferences popover, with two
// tabs on genuinely different persistence paths. Graph tab: browser
// localStorage, per-install rather than per-project — unlike every node's
// own settingsJson (which travels inside the saved .patchy project file
// via the Bridge), these preferences stay on this machine and don't
// follow the project when it's shared or opened elsewhere. Audio tab
// (standalone only): sent straight to the backend audio engine via
// Bridge.setAudioEngineSettings, with no undo/redo integration at all —
// sample rate/buffer size/mute-feedback are host/device state, not part
// of the document, so Undo doesn't conceptually apply to them the way it
// does to graph edits.

// ── Graph preferences ─────────────────────────────────────────────────────────
export interface GraphPreferences {
  invertZoom: boolean;
}

export const DEFAULT_PREFS: GraphPreferences = { invertZoom: false };
const STORAGE_KEY = 'nodegraph_prefs';

export function loadPrefs(): GraphPreferences {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return { ...DEFAULT_PREFS, ...JSON.parse(raw) };
  } catch {}
  return { ...DEFAULT_PREFS };
}

export function savePrefs(prefs: GraphPreferences) {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(prefs)); } catch {}
}

// ── Shared UI components ──────────────────────────────────────────────────────
function SectionHeader ({ label }: { label: string }) {
  return (
    <div style={{
      fontSize: 9, fontWeight: 700, letterSpacing: '0.12em',
      textTransform: 'uppercase', color: 'var(--text-muted)',
      borderBottom: '1px solid var(--border)', paddingBottom: 5,
      marginBottom: 8, marginTop: 12,
    }}>{label}</div>
  );
}

function ToggleRow ({ label, desc, value, onChange, disabled = false }: {
  label: string; desc?: string; value: boolean;
  onChange: (v: boolean) => void; disabled?: boolean;
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'flex-start',
                  justifyContent: 'space-between', gap: 12, marginBottom: 10,
                  opacity: disabled ? 0.45 : 1 }}>
      <div style={{ flex: 1 }}>
        <div style={{ fontSize: 11, color: 'var(--text)', marginBottom: desc ? 2 : 0 }}>{label}</div>
        {desc && <div style={{ fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.4 }}>{desc}</div>}
      </div>
      <div onClick={() => !disabled && onChange(!value)} style={{
        width: 32, height: 18, borderRadius: 9, flexShrink: 0,
        background: value ? 'var(--accent)' : 'var(--border)',
        position: 'relative', cursor: 'pointer', transition: 'background 0.2s',
        boxShadow: value ? '0 0 8px var(--accent)66' : 'none',
      }}>
        <div style={{
          position: 'absolute', top: 2, left: value ? 16 : 2,
          width: 14, height: 14, borderRadius: '50%',
          background: 'var(--text)', transition: 'left 0.2s',
          boxShadow: '0 1px 3px rgba(0,0,0,0.4)',
        }} />
      </div>
    </div>
  );
}

function SelectRow ({ label, value, options, onChange }: {
  label: string; value: string | number;
  options: (string | number)[]; onChange: (v: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Element)) setOpen(false);
    };
    document.addEventListener('mousedown', close);
    return () => document.removeEventListener('mousedown', close);
  }, [open]);

  return (
    <div style={{ display: 'flex', alignItems: 'center',
                  justifyContent: 'space-between', gap: 12, marginBottom: 10,
                  position: 'relative' }} ref={ref}>
      <div style={{ fontSize: 11, color: 'var(--text)' }}>{label}</div>
      <div
        onClick={() => setOpen(v => !v)}
        style={{
          display: 'flex', alignItems: 'center', gap: 6,
          background: 'var(--surface)', border: '1px solid var(--border)',
          borderRadius: 'var(--radius)', color: 'var(--text)',
          fontSize: 10, padding: '4px 8px', cursor: 'pointer',
          fontFamily: "'JetBrains Mono', monospace",
          minWidth: 90, justifyContent: 'space-between',
          userSelect: 'none',
        }}
      >
        <span>{String(value)}</span>
        <svg width="8" height="5" viewBox="0 0 8 5" style={{ opacity: 0.5, flexShrink: 0 }}>
          <polygon points="0,0 8,0 4,5" fill="currentColor" />
        </svg>
      </div>
      {open && (
        <div style={{
          position: 'absolute', top: '100%', right: 0, zIndex: 1000,
          background: 'var(--surface2)', border: '1px solid var(--border-hi)',
          borderRadius: 'var(--radius)', boxShadow: '0 8px 24px rgba(0,0,0,.7)',
          maxHeight: 200, overflowY: 'auto', minWidth: 100,
          marginTop: 2,
        }}>
          {options.map(o => (
            <div
              key={o}
              onClick={() => { onChange(String(o)); setOpen(false); }}
              style={{
                padding: '5px 10px', fontSize: 10, cursor: 'pointer',
                color: String(o) === String(value) ? 'var(--accent)' : 'var(--text)',
                background: String(o) === String(value) ? 'var(--surface)' : 'transparent',
                fontFamily: "'JetBrains Mono', monospace",
              }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--surface)')}
              onMouseLeave={e => (e.currentTarget.style.background =
                String(o) === String(value) ? 'var(--surface)' : 'transparent')}
            >{String(o)}</div>
          ))}
        </div>
      )}
    </div>
  );
}

// ── Audio tab (standalone only) ───────────────────────────────────────────────
function AudioTab({ settings: initSettings }: { settings: AudioSettings | null }) {
  const [settings, setSettings] = useState<AudioSettings | null>(initSettings);

  useEffect(() => {
    if (initSettings) setSettings(initSettings);
  }, [initSettings]);

  if (!settings) return (
    <div style={{ fontSize: 10, color: 'var(--text-muted)', padding: '12px 0' }}>
      Loading audio settings...
    </div>
  );

  const apply = (patch: Partial<AudioSettings>) => {
    const next = { ...settings, ...patch };
    setSettings(next);
    Bridge.setAudioEngineSettings(next.sampleRate, next.bufferSize, next.muteFeedback);
  };

  const srOptions = settings.availableSampleRates.length > 0
    ? settings.availableSampleRates
    : [44100, 48000, 88200, 96000, 176400, 192000];

  const bsOptions = settings.availableBufferSizes.length > 0
    ? settings.availableBufferSizes
    : [64, 128, 256, 512, 1024, 2048];

  return (
    <>
      <SectionHeader label="Audio Engine" />
      <SelectRow
        label="Sample Rate"
        value={settings.sampleRate}
        options={srOptions}
        onChange={v => apply({ sampleRate: Number(v) })}
      />
      <SelectRow
        label="Buffer Size"
        value={settings.bufferSize}
        options={bsOptions}
        onChange={v => apply({ bufferSize: Number(v) })}
      />
      <SectionHeader label="Protection" />
      <ToggleRow
        label="Mute feedback"
        desc="Mutes audio input when the same device is used for output, preventing feedback loops."
        value={settings.muteFeedback}
        onChange={v => apply({ muteFeedback: v })}
      />
    </>
  );
}

// ── Graph tab ─────────────────────────────────────────────────────────────────
function GraphTab ({ prefs, onChange }: {
  prefs: GraphPreferences; onChange: (p: GraphPreferences) => void;
}) {
  const patch = (partial: Partial<GraphPreferences>) => {
    const next = { ...prefs, ...partial };
    onChange(next);
    savePrefs(next);
  };
  const { isStandalone, dawLoopbackEnabled, setDawLoopback, dawHostEnabled, setDawHost } = useContext(DawContext);

  return (
    <>
      {!isStandalone && (
        <>
          <SectionHeader label="DAW Routing" />
          <ToggleRow
            label="Enable DAW loopback"
            desc="Allow AudioOUT to route back to the DAW track. Risk of feedback loop!"
            value={dawLoopbackEnabled}
            onChange={setDawLoopback}
          />
          <ToggleRow
            label="Enable DAW host devices"
            desc="Allow selection of your DAW's own virtual audio devices. May cause signal doubling!"
            value={dawHostEnabled}
            onChange={setDawHost}
          />
        </>
      )}
      <SectionHeader label="Navigation" />
      {/* Genuinely unimplemented, not just this toggle disabled — invertZoom
          is declared in GraphPreferences and stored via localStorage, but
          nothing anywhere else in the codebase reads it back to actually
          affect zoom direction yet. */}
      <ToggleRow
        label="Invert zoom direction"
        desc="Coming soon — macOS trackpad gestures require deeper integration"
        value={false}
        onChange={() => {}}
        disabled
      />
    </>
  );
}

// ── Main panel ────────────────────────────────────────────────────────────────
export default function PreferencesPanel ({
  prefs, onChange, onClose, isStandalone, audioSettings, excludeRef,
}: {
  prefs:         GraphPreferences;
  onChange:      (p: GraphPreferences) => void;
  onClose:       () => void;
  excludeRef?:   React.RefObject<HTMLElement | null>;
  isStandalone:  boolean;
  audioSettings: AudioSettings | null;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const [activeTab, setActiveTab] = useState<'graph' | 'audio'>('graph');
  const { setHint } = useContext(HintContext);

  // excludeRef points at the toggle button that opens this panel (App.tsx's
  // prefsBtnRef) — without excluding it, clicking that button to close an
  // already-open panel would register as an outside click first (closing
  // it), then the button's own onClick would immediately reopen it.
  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (excludeRef?.current?.contains(e.target as Element)) return;
      if (ref.current && !ref.current.contains(e.target as Element)) onClose();
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [onClose]);

  const tabStyle = (active: boolean): React.CSSProperties => ({
    fontSize: 10, fontWeight: 700, letterSpacing: '0.08em',
    textTransform: 'uppercase', padding: '5px 10px',
    cursor: 'pointer', borderBottom: active ? '2px solid var(--accent)' : '2px solid transparent',
    color: active ? 'var(--accent)' : 'var(--text-muted)',
    transition: 'all 0.15s',
    fontFamily: "'JetBrains Mono', monospace",
  });

  return (
    <div
      ref={ref}
      onMouseDown={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onMouseEnter={() => setHint({ title: 'Preferences', body: 'Configure graph and audio settings.' })}
      onMouseLeave={() => setHint(null)}
      style={{
        position: 'absolute', top: 48, right: 0,
        width: 300, zIndex: 99999,
        background: 'var(--surface2)',
        border: '1px solid var(--border-hi)',
        borderRadius: 'var(--radius)',
        boxShadow: '0 8px 32px rgba(0,0,0,.7)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}
    >
      {/* Header */}
      <div style={{ display: 'flex', justifyContent: 'space-between',
                    alignItems: 'center', padding: '12px 14px 0' }}>
        <div style={{
          fontSize: 12, fontWeight: 700, color: 'var(--text)',
          letterSpacing: '0.08em', fontFamily: "'Syne', sans-serif",
        }}>PATCHY PREFERENCES</div>
        <button onClick={onClose} style={{
          background: 'none', border: 'none', color: 'var(--text-muted)',
          cursor: 'pointer', fontSize: 14, padding: '0 2px', lineHeight: 1,
        }}>✕</button>
      </div>

      {/* Tabs */}
      <div style={{ display: 'flex', borderBottom: '1px solid var(--border)',
                    padding: '0 14px', marginTop: 8 }}>
          <div style={tabStyle(activeTab === 'graph')} onClick={() => setActiveTab('graph')}>
          Graph
        </div>
        {isStandalone && (
          <div style={tabStyle(activeTab === 'audio')} onClick={() => setActiveTab('audio')}>
            Audio
          </div>
        )}
      </div>

      {/* Tab content */}
      <div style={{ padding: '0 14px 14px' }}>
        {activeTab === 'audio' && isStandalone && <AudioTab settings={audioSettings} />}
        {activeTab === 'graph' && <GraphTab prefs={prefs} onChange={onChange} />}
      </div>
    </div>
  );
}
