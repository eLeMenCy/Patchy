// ValueConsoleNode.tsx — v0.0.992: the Value Console Pax's node (user,
// 2026-11-20: the Value hub needs a way to inject values to test with).
//
// One row per fader: its number, the fader (0–1 position), the value it sends
// (Low…High), a STEP button (jumps 0 ↔ 1) and RAMP (sweeps on its own — the
// fader then follows the value sent, read live from the Pax). Each row has
// its own Value Out port. 1 fader to start, "+ fader" up to Preferences →
// Value Console (default 4; like the MIDI Keyboard's faders, which start at 0); "− fader" removes the last
// one, unless a wire uses its port.
// The ⚙ panel: per fader, its Low / High range (two heads) and the ramp's
// period. Settings: flat array, one value per Pax parameter (index 0 = count,
// then 6 per fader: Value, Low, High, Ramp, Period, Out (read-only)).

import { useState, useEffect, useCallback, useContext, useRef } from 'react';
import { NodeProps, useEdges } from '@xyflow/react';
import { Bridge } from './Bridge';
import ValueBox from './ValueBox';
import { NodeHandle, useNodeCollapsed, useNodeDisabled, NodeHeaderButton, NodeCollapseArrow, settingsPanelStyle,
         EditableTitle, commitModelName, SettingsPanelHeader, SliderRow, _paxInfoMap, detectPaxTheme, readoutBoxStyle } from './NodeUtils';
import { HintContext } from './HintPanel';
import { RangeDual } from './ValueHubNode';
import { Settings, X, Power } from 'lucide-react';
import { perfCount } from './Perf';   // v0.0.994

const FADERS = 16, PER = 6;
const B = (k: number) => 1 + k * PER;                     // a fader's first parameter
const RAMPS_ON_OPEN = 1 + FADERS * PER;   // v0.0.996 — 0: a file / session opens with the ramps off (host)
const DEFAULTS: number[] = [1, ...Array.from ({ length: FADERS }, () => [0, 0, 1, 0, 4, 0]).flat (), 0];
let maxValueFaders = 4;
export const setValueFaderPrefs = (max: number) => { maxValueFaders = Math.max (1, Math.min (FADERS, Math.round (max || 4))); };
const _settingsOpen = new Map<string, boolean>();
const num = (v: number) => (Math.round (v * 1000) / 1000).toString ();

export default function ValueConsoleNode ({ id, data, selected }: NodeProps) {
  perfCount ('render ValueConsoleNode');   // v0.0.994 — overlay: how often it re-renders
  const d = data as any;
  const { setHint } = useContext (HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, d._forceCollapsed);
  const { disabled, toggleDisabled }   = useNodeDisabled (id, d.disabled);
  const [showSettings, setShowSettings] = useState (() => _settingsOpen.get (id) ?? false);
  const [vals, setVals] = useState<number[]> (DEFAULTS);
  const [outs, setOuts] = useState<number[]> ([]);
  const lastOuts = useRef (0);   // v0.0.994
  const edges = useEdges ();
  const accent = detectPaxTheme (d.ports ?? [], d.paxName ? _paxInfoMap.get (d.paxName)?.colourCategory : undefined).accent;

  useEffect (() => {
    let raw: number[] | null = null;
    try { raw = d.settingsJson ? JSON.parse (d.settingsJson) as number[] : null; } catch { raw = null; }
    const v = DEFAULTS.map ((def, i) => (raw && typeof raw[i] === 'number') ? raw[i] : def);
    setVals (v);
    v.forEach ((x, i) => { if (i === 0 || (i - 1) % PER !== 5) Bridge.setPaxParameter (id, i, x); });
  }, [d.settingsJson]);   // eslint-disable-line react-hooks/exhaustive-deps

  // the values sent (read-only "Out" per fader): the fader follows them while it ramps
  useEffect (() => Bridge.onPortActivity ((entries: any[]) => {
    const e = entries.find (x => x.id === id);
    if (! e?.paxReadOnly) return;
    const next: number[] = [];
    for (const r of e.paxReadOnly) if (r.index >= 1 && (r.index - 1) % PER === 5) next[(r.index - 1 - 5) / PER] = r.value;
    // v0.0.994 — at most 15 redraws a second (it redrew the node on every
    // push while ramping); the sent values themselves are unaffected
    const now = performance.now ();
    if (now - lastOuts.current < 66) return;
    lastOuts.current = now;
    setOuts (prev => prev.length === next.length && prev.every ((x, k) => x === next[k]) ? prev : next);
  }), [id]);

  const setParam = useCallback ((idx: number, v: number, commit: boolean) => {
    setVals (prev => { const next = [...prev]; next[idx] = v; Bridge.setNodeSettings (id, next); return next; });
    Bridge.setPaxParameter (id, idx, v);
    if (commit) Bridge.commitNodeSettings (id);
  }, [id]);

  const count = Math.max (1, Math.min (FADERS, Math.round (vals[0] ?? 1)));
  const ports = ((d.ports ?? []) as { id: string; label: string }[]).filter (p => ! p.id.endsWith ('_in'));
  const portOf = (k: number) => ports[k];
  const wired = (k: number) => { const p = portOf (k); return !! p && edges.some (e => e.source === id && e.sourceHandle === p.id); };
  const customName: string = d.customName ?? '';

  const resetAll = () => {
    setVals (DEFAULTS);
    Bridge.setNodeSettings (id, DEFAULTS);
    DEFAULTS.forEach ((x, i) => { if (i === 0 || (i - 1) % PER !== 5) Bridge.setPaxParameter (id, i, x); });
    Bridge.commitNodeSettings (id);
  };
  const smallBtn = (on: boolean): React.CSSProperties => ({
    fontSize: 8, fontWeight: 700, padding: '1px 4px', borderRadius: 2, cursor: 'pointer', userSelect: 'none',
    background: on ? accent : 'var(--bg)', color: on ? '#000' : 'var(--text-muted)', border: `1px solid ${on ? accent : 'var(--border)'}`,
  });

  return (
    <div style={{
      display: 'inline-block', minWidth: 250, background: 'var(--surface)',
      border: `1px solid ${selected ? accent : 'var(--border)'}`, borderTop: `3px solid ${accent}`,
      borderRadius: 'var(--radius)', fontFamily: "'JetBrains Mono', monospace",
      filter: disabled ? 'grayscale(0.8) opacity(0.55)' : 'none', transition: 'filter .15s', position: 'relative',
    }}>
      {Array.from ({ length: collapsed ? 0 : count }, (_, k) => portOf (k) && (
        <NodeHandle key={portOf (k).id} nodeId={id} portId={portOf (k).id} label={portOf (k).label} direction="out"
          colour="var(--value)" index={0} total={1} anchor={`f${k}`} />
      ))}

      <div style={{ display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4, cursor: 'pointer',
                    borderBottom: collapsed ? 'none' : `1px solid color-mix(in srgb, ${accent} 27%, transparent)` }}
           onDoubleClick={toggleCollapsed}>
        <NodeCollapseArrow collapsed={collapsed} accent={accent} />
        <div style={{ flex: 1, minWidth: 0 }}>
          <EditableTitle display={customName || 'Value Console'} value={customName} placeholder="Value Console"
            onCommit={commitModelName (id)}
            textStyle={{ fontSize: '11px', fontWeight: 700, color: accent, letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
                         whiteSpace: 'nowrap', textTransform: 'uppercase', userSelect: 'none' }} />
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={toggleDisabled} active={! disabled} activeAccent={accent}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Disable / Enable Node', body: 'A disabled console sends nothing.' }), onMouseLeave: () => setHint (null) }}>
            <Power size={11} />
          </NodeHeaderButton>
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={() => setShowSettings (v => { const next = ! v; _settingsOpen.set (id, next); return next; })}
            active={showSettings} activeAccent={accent}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Settings', body: 'Each fader\'s Low / High range and its ramp period.' }), onMouseLeave: () => setHint (null) }}>
            <Settings size={11} />
          </NodeHeaderButton>
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={() => Bridge.removeNode (id)}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Delete node', body: 'Remove this node.' }), onMouseLeave: () => setHint (null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {! collapsed && (
        <div className="nodrag" onMouseDown={e => e.stopPropagation ()} style={{ padding: '6px 12px 6px 8px', display: 'flex', flexDirection: 'column', gap: 4 }}>
          {Array.from ({ length: count }, (_, k) => {
            const b = B (k), ramp = (vals[b + 3] ?? 0) >= 0.5;
            const lo = vals[b + 1] ?? 0, hi = vals[b + 2] ?? 1;
            const sent = outs[k];
            const pos = ramp && sent !== undefined && hi !== lo ? (sent - lo) / (hi - lo) : (vals[b] ?? 0);
            return (
              <div key={k} data-port-anchor={`f${k}`} style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
                <span style={{ fontSize: 8, color: 'var(--text-muted)', minWidth: 12, textAlign: 'right' }}>{k + 1}</span>
                <input type="range" min={0} max={1} step={0.001} value={Math.max (0, Math.min (1, pos))} disabled={ramp}
                  onChange={e => setParam (b, Number (e.target.value), false)}
                  onMouseUp={() => Bridge.commitNodeSettings (id)}
                  onDoubleClick={() => setParam (b, 0, true)}
                  style={{ flex: 1, minWidth: 0, ['--thumb-color' as any]: accent, opacity: ramp ? 0.7 : 1 }} />
                <ValueBox value={lo + (vals[b] ?? 0) * (hi - lo)} min={Math.min (lo, hi)} max={Math.max (lo, hi)} step={0.01} accent={accent}
                  format={v => num (ramp && sent !== undefined ? sent : v)}
                  onLive={v => hi !== lo && setParam (b, Math.max (0, Math.min (1, (v - lo) / (hi - lo))), false)}
                  onCommit={v => { if (hi !== lo) setParam (b, Math.max (0, Math.min (1, (v - lo) / (hi - lo))), true); }}
                  width={40} style={{ ...readoutBoxStyle (accent), textAlign: 'center' }} />
                <span style={smallBtn (false)} onClick={() => setParam (b, (vals[b] ?? 0) >= 0.5 ? 0 : 1, true)}
                  onMouseEnter={() => setHint ({ title: 'Step', body: 'Jumps the fader to the other end: Low ↔ High (sends at once).' })}
                  onMouseLeave={() => setHint (null)}>STEP</span>
                <span style={smallBtn (ramp)} onClick={() => setParam (b + 3, ramp ? 0 : 1, true)}
                  onMouseEnter={() => setHint ({ title: 'Ramp', body: `Sweeps Low → High → Low on its own, ${num (vals[b + 4] ?? 4)} s per sweep (⚙ to change).` })}
                  onMouseLeave={() => setHint (null)}>RAMP</span>
              </div>
            );
          })}
          <div style={{ display: 'flex', justifyContent: 'center', gap: 6, marginTop: 2 }}>
            {count < maxValueFaders && (
              <span style={smallBtn (false)} onClick={() => setParam (0, count + 1, true)}
                onMouseEnter={() => setHint ({ title: '+ fader', body: `Adds a fader with its own Value Out (up to ${maxValueFaders}: Preferences → Value Console).` })}
                onMouseLeave={() => setHint (null)}>+ FADER</span>
            )}
            {count > 1 && (
              <span style={{ ...smallBtn (false), opacity: wired (count - 1) ? 0.4 : 1, cursor: wired (count - 1) ? 'not-allowed' : 'pointer' }}
                onClick={() => { if (! wired (count - 1)) setParam (0, count - 1, true); }}
                onMouseEnter={() => setHint ({ title: '− fader', body: wired (count - 1) ? 'The last fader\'s port is wired: remove its wire first.' : 'Removes the last fader.' })}
                onMouseLeave={() => setHint (null)}>− FADER</span>
            )}
          </div>
        </div>
      )}

      {showSettings && ! collapsed && (
        <div style={settingsPanelStyle}>
          <SettingsPanelHeader title="VALUE CONSOLE" onReset={resetAll}
            onClose={() => { _settingsOpen.set (id, false); setShowSettings (false); }} />
          {/* v0.0.996 — user: ramps off when a file opens, unless asked for */}
          <div className="nodrag" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '2px 0 5px', borderBottom: '1px solid var(--border)' }}
            onMouseEnter={() => setHint ({ title: 'Ramps on open', body: 'Off: when a file or a session opens, every ramp of this console starts off — nothing moves until you start it. On: ramps that were running start again. Undo / redo never stops a ramp.' })}
            onMouseLeave={() => setHint (null)}>
            <span style={{ fontSize: 8, color: 'var(--text-muted)' }}>Ramps on open</span>
            <span onClick={() => setParam (RAMPS_ON_OPEN, (vals[RAMPS_ON_OPEN] ?? 0) >= 0.5 ? 0 : 1, true)}
              style={smallBtn ((vals[RAMPS_ON_OPEN] ?? 0) >= 0.5)}>{(vals[RAMPS_ON_OPEN] ?? 0) >= 0.5 ? 'ON' : 'OFF'}</span>
          </div>
          {Array.from ({ length: count }, (_, k) => {
            const b = B (k);
            return (
              <div key={k} style={{ padding: '5px 0', borderBottom: k < count - 1 ? '1px solid var(--border)' : 'none' }}>
                <RangeDual label={`Fader ${k + 1}`} lo={vals[b + 1] ?? 0} hi={vals[b + 2] ?? 1} accent={accent}
                  onChange={(w, v) => setParam (w === 'lo' ? b + 1 : b + 2, v, false)}
                  onSet={(lo, hi) => { setParam (b + 1, lo, false); setParam (b + 2, hi, false); }}
                  onCommit={() => Bridge.commitNodeSettings (id)} />
                <div style={{ height: 4 }} />{/* a little air between the range and the period (user) */}
                <SliderRow label="Ramp period" value={vals[b + 4] ?? 4} min={0.1} max={60} step={0.1} format={v => `${num (v)} s`} color={accent}
                  onChange={v => setParam (b + 4, v, false)} onDoubleClick={() => setParam (b + 4, 4, false)}
                  onCommit={() => Bridge.commitNodeSettings (id)} />
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
