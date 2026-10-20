// ValueBox.tsx — v0.0.942: the MIDI Morpher's value control, for every node.
//
//   drag up / down     scrub (⇧ Shift = fine steps)
//   ⌥ Option + drag    with a partner (Low / High pair): both ends together
//   double-click       type a value (Enter keeps, Esc cancels; "min" / "max" work)
//
// Two ways to report:
//   onLive   (optional) — every step while dragging (sliders' readouts, local display)
//   onCommit           — once: at the end of a drag (if it changed) or a typed value
// Fields that must not change under the pointer at each step (a network port
// rebinding its socket) just leave onLive out: the box shows the dragged value
// and commits on release.
import { useEffect, useRef, useState } from 'react';

export interface ValueBoxProps {
  value:     number;
  min:       number;
  max:       number;
  step?:     number;             // default 1 (whole numbers)
  fineStep?: number;             // with ⇧ — default step / 10 (whole numbers: 1)
  log?:      boolean;            // proportional steps (Hz): drag moves by a ratio
  coarse?:   number;             // steps per tick without ⇧ (default: 10 for ranges over 2000 steps, else 1)
  decimals?: number;             // shown / typed precision (default: from step)
  unit?:     string;             // shown after the value ("ms", "Hz"…)
  format?:   (v: number) => string;   // replaces the default display
  parse?:    (t: string) => number;   // typed text → value (NaN = ignore)
  minWord?:  string;             // shown instead of min (e.g. "Min", "Off")
  maxWord?:  string;             // shown instead of max
  placeholder?: string;          // shown when value is NaN
  onLive?:   (v: number) => void;
  onCommit:  (v: number) => void;
  onRelease?: () => void;        // drag ended (after onCommit) — e.g. close an undo step
  onEnter?:  (v: number) => void;   // typed value confirmed with Return (after onCommit) — e.g. Send
  onLinked?: (v: number) => void;   // ⌥ + drag: set both ends of a pair to v (live)
  linkMin?:  number;             // bounds while linked (a pair's own min / max is the partner)
  linkKey?:  'alt' | 'cmd';      // v0.0.943 — key that links: ⌥ (Morpher-style ranges) or ⌘ / Ctrl (frequency bands: move the band)
  linkMax?:  number;
  title?:    string;
  accent?:   string;
  width?:    number | string;
  style?:    React.CSSProperties;   // merged over the default box style
  dim?:      boolean;
  disabled?: boolean;
}

const decimalsOf = (step: number) => {
  if (step >= 1) return 0;
  const s = String (step);
  return s.includes ('.') ? s.split ('.')[1].length : 2;
};

export default function ValueBox ({
  value, min, max, step = 1, fineStep, log = false, coarse, decimals, unit, format, parse,
  minWord, maxWord, placeholder, onLive, onCommit, onRelease, onLinked, onEnter, linkMin, linkMax, linkKey = 'alt',
  title, accent = 'var(--accent)', width = 52, style, dim, disabled,
}: ValueBoxProps) {
  const dec   = decimals ?? decimalsOf (step);
  const fine  = fineStep ?? (step >= 1 ? 1 : step / 10);
  const clamp = (v: number) => Math.max (min, Math.min (max, v));
  const round = (v: number, s: number) => {
    const r = Math.round (v / s) * s;
    return Number (r.toFixed (Math.max (dec, decimalsOf (s))));
  };

  // While dragging the box shows its own value (the owner may only learn it
  // at the end); otherwise the owner's value.
  const [dragV,   setDragV]   = useState<number | null> (null);
  const [editing, setEditing] = useState (false);
  const [draft,   setDraft]   = useState ('');
  const done = useRef (false);
  const shownV = dragV ?? value;

  // Latest callbacks for the window listeners that live for the whole drag
  const cbs = useRef ({ onLive, onCommit, onRelease, onLinked });
  cbs.current = { onLive, onCommit, onRelease, onLinked };
  useEffect (() => () => { cleanup.current?.(); }, []);
  const cleanup = useRef<(() => void) | null> (null);

  const text = (v: number) => {
    if (isNaN (v)) return placeholder ?? '';
    if (format) return format (v);
    if (minWord && v <= min) return minWord;
    if (maxWord && v >= max) return maxWord;
    return v.toFixed (dec) + (unit ? ` ${unit}` : '');
  };

  const commitDraft = (viaEnter = false) => {
    if (done.current) return;
    done.current = true;
    const t = draft.trim().toLowerCase();
    let n = parse ? parse (draft)
          : t === 'max' || (maxWord && t === maxWord.toLowerCase()) ? max
          : t === 'min' || (minWord && t === minWord.toLowerCase()) ? min
          : parseFloat (t.replace (',', '.')) * (/^-?[\d.,]+\s*k/.test (t) ? 1000 : 1);   // "2k" = 2000 (Hz)
    if (! isNaN (n)) { n = clamp (round (n, fine)); cbs.current.onCommit (n); if (viaEnter) onEnter?.(n); }
    setEditing (false);
  };

  const onMouseDown = (e: React.MouseEvent) => {
    if (disabled || e.button !== 0) return;
    // No stopPropagation (see the Morpher's RangeValue): it would also stop the
    // native event at the root, and this drag's window mouseup would never
    // come. `nodrag` keeps ReactFlow from moving the node.
    e.preventDefault();
    // With onEnter the box takes the keyboard focus (preventDefault above
    // blocks it): Return then confirms the dragged value too (e.g. Send)
    if (onEnter) (e.currentTarget as HTMLElement).focus();
    // Unset (NaN): start from the hint's number ("e.g. 8000" → 8000), else 0 —
    // not from min (the MQTT payload's min is -1e9)
    const hinted = parseFloat ((placeholder ?? '').match (/-?\d+(\.\d+)?/)?.[0] ?? '');
    const start = ! isNaN (value) ? value : clamp (isNaN (hinted) ? 0 : hinted);
    const d = { y: e.clientY, v: start, cur: start, linked: false, lastY: e.clientY, moved: false, shift: e.shiftKey };

    const setLinked = (on: boolean, y: number) => {
      if (! cbs.current.onLinked || d.linked === on) return;
      d.linked = on; d.y = y; d.v = d.cur;
      if (on) cbs.current.onLinked (d.cur);
    };
    const setShift = (on: boolean, y: number) => {
      if (d.shift === on) return;
      d.shift = on; d.y = y; d.v = d.cur;   // re-anchor: no jump when switching speed
    };
    const linkHeld = (ev: { altKey: boolean; metaKey: boolean; ctrlKey: boolean }) => linkKey === 'cmd' ? (ev.metaKey || ev.ctrlKey) : ev.altKey;
    setLinked (linkHeld (e), e.clientY);

    const apply = (y: number) => {
      const ticks = Math.round ((d.y - y) / 3);
      let next: number;
      if (log) {
        const ratio = d.shift ? 1.005 : 1.03;          // per 3 px
        next = d.v > 0 ? d.v * Math.pow (ratio, ticks) : min;
        next = round (next, fine);
      } else {
        const big = d.shift ? 1 : coarse ?? ((max - min) / step > 2000 ? 10 : 1);   // long ranges (ports)
        next = round (d.v + ticks * (d.shift ? fine : step * big), d.shift ? fine : step);
      }
      next = d.linked ? Math.max (linkMin ?? min, Math.min (linkMax ?? max, next)) : clamp (next);
      if (next === d.cur) return;
      d.cur = next; d.moved = true;
      setDragV (next);
      if (d.linked) cbs.current.onLinked?.(next);
      else          cbs.current.onLive?.(next);
    };
    const move = (ev: MouseEvent) => {
      d.lastY = ev.clientY;
      setLinked (linkHeld (ev), ev.clientY);
      setShift  (ev.shiftKey, ev.clientY);
      apply (ev.clientY);
    };
    const key = (ev: KeyboardEvent) => {
      if (linkKey === 'alt' ? ev.key === 'Alt' : ev.key === 'Meta' || ev.key === 'Control') setLinked (ev.type === 'keydown', d.lastY);
      if (ev.key === 'Shift') setShift  (ev.type === 'keydown', d.lastY);
    };
    const up = () => {
      cleanup.current?.();
      setDragV (null);
      if (d.moved && ! d.linked) cbs.current.onCommit (d.cur);
      else if (d.moved && d.linked) cbs.current.onLinked?.(d.cur);
      cbs.current.onRelease?.();
    };
    cleanup.current = () => {
      window.removeEventListener ('mousemove', move, true);
      window.removeEventListener ('mouseup',   up,   true);
      window.removeEventListener ('keydown',   key,  true);
      window.removeEventListener ('keyup',     key,  true);
      cleanup.current = null;
    };
    window.addEventListener ('mousemove', move, true);
    window.addEventListener ('mouseup',   up,   true);
    window.addEventListener ('keydown',   key,  true);
    window.addEventListener ('keyup',     key,  true);
  };

  const box: React.CSSProperties = {
    width, boxSizing: 'border-box', textAlign: 'center', fontSize: 10, padding: '2px 4px', borderRadius: 3,
    border: '1px solid var(--border)', background: 'var(--surface)',
    color: dim ? 'var(--text-muted)' : 'var(--text)', fontFamily: "'JetBrains Mono', monospace",
    fontVariantNumeric: 'tabular-nums', opacity: dim || disabled ? 0.5 : 1,
    whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
    ...style,
  };
  const tip = title ?? `Drag up/down (⇧ fine)${! onLinked ? '' : linkKey === 'cmd' ? ' • ⌘ + drag: moves the whole band' : ' • ⌥ Option + drag: low and high together'} • double-click to type`;

  return editing ? (
    <input autoFocus value={draft} className="nodrag"
      onChange={e => setDraft (e.target.value)}
      onBlur={() => commitDraft()}
      onKeyDown={e => { e.stopPropagation(); if (e.key === 'Enter') commitDraft (true); if (e.key === 'Escape') { done.current = true; setEditing (false); } }}
      onDoubleClick={e => e.stopPropagation()}
      style={{ ...box, outline: 'none', borderColor: accent, cursor: 'text' }} />
  ) : (
    <span className="nodrag" title={tip}
      tabIndex={onEnter ? 0 : undefined}
      onKeyDown={onEnter ? (e => { if (e.key === 'Enter' && ! isNaN (value)) { e.preventDefault(); e.stopPropagation(); onEnter (value); } }) : undefined}
      onMouseDown={onMouseDown}
      onDoubleClick={e => {
        e.stopPropagation();
        if (disabled) return;
        done.current = false;
        setDraft (isNaN (value) ? '' : format ? format (value) : value.toFixed (dec));
        setEditing (true);
      }}
      style={{ ...box, cursor: disabled ? 'default' : 'ns-resize', userSelect: 'none', display: 'inline-block', outline: 'none',
               borderColor: dragV !== null ? accent : (box.borderColor ?? 'var(--border)') }}>
      {text (shownV)}
    </span>
  );
}
