// Patchy — MIDI Morpher: Basic mode (v0.0.927)
//
// Basic is a second FACE of the same rule, not a second rule: it reads and
// writes the very same MorpherRule fields as Advanced. Users pick friendly
// things ("Note On C3 on ch 3 → Knob-slider (CC) 74, value = velocity") and
// Basic works out the Data1/Data2 ranges and Pull flags itself.
//
// toAdvanced(): Basic → rule.  fromAdvanced(): rule → Basic, or null when
// the rule can't be expressed in Basic ("too complex": Basic then shows it
// read-only with an "Edit in Advanced" button). fromAdvanced() is checked by
// converting back: only an exact round trip counts as Basic.

import { MorpherRule, Range, isFull, rHi, data1Max, PITCH } from './MorpherCore';

// ── Message kinds (same codes as the engine: 0 Any, 1 Note Off … 7 Pitch) ──
export const NOTE_OFF = 1, NOTE_ON = 2, POLY_AT = 3, CC = 4, PROGRAM = 5, CHAN_AT = 6;

/** Friendly names, in the order Basic's pickers list them. */
export const BASIC_KINDS: { code: number; name: string }[] = [
  { code: NOTE_ON, name: 'Note On' },
  { code: NOTE_OFF, name: 'Note Off' },
  { code: CC,       name: 'Knob-slider (CC)' },
  { code: PITCH,    name: 'Pitch bend' },
  { code: PROGRAM,  name: 'Program change' },
  { code: CHAN_AT,  name: 'Aftertouch (channel)' },
  { code: POLY_AT,  name: 'Aftertouch (poly)' },
  { code: 0,        name: 'Any event' },
];
const SHORT: Record<number, string> = {
  0: 'Any event', [NOTE_OFF]: 'Note Off', [NOTE_ON]: 'Note On', [POLY_AT]: 'Poly AT',
  [CC]: 'CC', [PROGRAM]: 'Program', [CHAN_AT]: 'Aftertouch', [PITCH]: 'Pitch bend',
};
export const kindShort = (k: number) => SHORT[k] ?? '?';

/** Has a "which one" number (note, CC number, program number) in Data1. */
export const hasNumber = (k: number) => k >= NOTE_OFF && k <= PROGRAM;
/** Where the "value" lives: 2 = Data2 (velocity, CC value, poly pressure),
 *  1 = Data1 (channel pressure, pitch bend), 0 = none (Program change). */
export const valueSlot = (k: number): 0 | 1 | 2 => (k === PROGRAM ? 0 : k === CHAN_AT || k === PITCH ? 1 : 2);
export const valueMax  = (k: number) => (valueSlot (k) === 1 ? data1Max (k) : 127);
const isNote = (k: number) => k === NOTE_ON || k === NOTE_OFF || k === POLY_AT;

/** What the incoming value is called in a sentence ("value = velocity"). */
export function valueSourceName (k: number): string {
  if (k === NOTE_ON || k === NOTE_OFF) return 'velocity';
  if (k === POLY_AT || k === CHAN_AT)  return 'pressure';
  if (k === CC)    return 'CC value';
  if (k === PITCH) return 'bend';
  return 'value';
}
/** What the outgoing value is called ("velocity 90", "bend 0–8192"). */
export function valueOutName (k: number): string {
  if (k === NOTE_ON || k === NOTE_OFF) return 'velocity';
  if (k === POLY_AT || k === CHAN_AT)  return 'pressure';
  if (k === PITCH) return 'bend';
  return 'value';
}

// ── Note names (same conventions as the MIDI Monitor) ──────────────────────
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export type NoteNames = 'yamaha' | 'roland';
const octOffset = (c: NoteNames) => (c === 'yamaha' ? -2 : -1);   // 60 = C3 / C4

export const noteName = (n: number, c: NoteNames) => `${NAMES[n % 12]}${Math.floor (n / 12) + octOffset (c)}`;

/** "C3", "c#3", "Db-1", or a plain number → 0..127, NaN if not a note. */
export function parseNote (text: string, c: NoteNames): number {
  const t = text.trim();
  if (/^\d+$/.test (t)) return parseInt (t, 10);
  const m = /^([A-Ga-g])([#b]?)(-?\d+)$/.exec (t);
  if (! m) return NaN;
  const base = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 }[m[1].toLowerCase() as 'c'] as number;
  const n = (parseInt (m[3], 10) - octOffset (c)) * 12 + base + (m[2] === '#' ? 1 : m[2] === 'b' ? -1 : 0);
  return n >= 0 && n <= 127 ? n : NaN;
}

/** How a number shows: note names for note kinds, plain numbers otherwise. */
export const numberText = (k: number, n: number, c: NoteNames) => (isNote (k) ? noteName (n, c) : String (n));

// ── The Basic rule ───────────────────────────────────────────────────────────
export type ValueChoice = 'same' | 'inverted' | 'fixed' | 'limited';

export interface BasicRule {
  inKind:  number;          // 0 = any event
  inNum:   number | null;   // which note / CC / program; null = any
  inCh:    number;          // 0 = Omni
  outKind: number;          // 0 = same kind
  outNum:  number | null;   // null = same number as the incoming one
  outCh:   number;          // 0 = same channel
  value:   ValueChoice;
  fixed:   number;
  limLo:   number;
  limHi:   number;
}

export const outKindOf = (b: BasicRule) => (b.outKind === 0 ? b.inKind : b.outKind);

/** Which value choices make sense for this rule. */
export function valueChoices (b: BasicRule): ValueChoice[] {
  const outType = outKindOf (b);
  if (valueSlot (outType) === 0) return [];           // Program change out: no value
  if (b.inKind !== 0 && valueSlot (b.inKind) === 0)   // Program change in: nothing to carry over
    return ['fixed'];
  return ['same', 'inverted', 'fixed', 'limited'];
}

/** Keep a Basic rule self-consistent after any pick. */
export function normaliseBasic (b: BasicRule): BasicRule {
  const r = { ...b };
  if (r.inKind === 0) { r.outKind = 0; r.outNum = null; r.inNum = null; }
  if (! hasNumber (r.inKind)) r.inNum = null;
  const outType = outKindOf (r);
  if (! hasNumber (outType)) r.outNum = null;
  else if (r.outNum === null && ! hasNumber (r.inKind)) r.outNum = 0;   // "same number" needs one coming in
  const choices = valueChoices (r);
  if (choices.length === 0) r.value = 'same';
  else if (! choices.includes (r.value)) r.value = choices[0];
  const vmax = valueMax (outType);
  r.fixed = Math.max (0, Math.min (vmax, r.fixed));
  r.limLo = Math.max (0, Math.min (vmax, r.limLo));
  r.limHi = Math.max (0, Math.min (vmax, r.limHi));
  return r;
}

export const DEFAULT_BASIC: BasicRule = {
  inKind: 0, inNum: null, inCh: 0, outKind: 0, outNum: null, outCh: 0,
  value: 'same', fixed: 127, limLo: 0, limHi: 127,
};

/** Basic → the engine's rule fields (keeps every other field of `base`). */
export function toAdvanced (b0: BasicRule, base: MorpherRule): MorpherRule {
  const b = normaliseBasic (b0);
  const outType = outKindOf (b);
  const outV = valueSlot (outType);
  const inV  = b.inKind === 0 ? 2 : valueSlot (b.inKind);
  const vmax = valueMax (outType);
  const full: Range = [0, -1];

  const valueRange: Range =
      b.value === 'inverted' ? [vmax, 0]
    : b.value === 'fixed'    ? [b.fixed, b.fixed]
    : b.value === 'limited'  ? [b.limLo, b.limLo > 0 && b.limHi >= vmax ? -1 : b.limHi]   // "20–Max" stays Max, like Advanced
    : full;
  const numberRange: Range = b.outNum === null ? full : [b.outNum, b.outNum];

  const r: MorpherRule = {
    ...base,
    inCh: b.inCh, inMsg: b.inKind,
    inD1: b.inNum === null ? full : [b.inNum, b.inNum], inD2: full,
    outCh: b.outCh, outMsg: b.outKind,
    outD1: full, outD2: full, outD1Pull: false, outD2Pull: false,
  };
  if (outV === 1)      { r.outD1 = valueRange; r.outD1Pull = inV === 2; }
  else if (outV === 2) { r.outD1 = numberRange; r.outD2 = valueRange; r.outD2Pull = inV === 1; }
  else                 { r.outD1 = numberRange; }
  return r;
}

const sameRange = (a: Range, b: Range) => a[0] === b[0] && a[1] === b[1];
function sameRule (a: MorpherRule, b: MorpherRule): boolean {
  return a.inCh === b.inCh && a.inMsg === b.inMsg && sameRange (a.inD1, b.inD1) && sameRange (a.inD2, b.inD2)
      && a.outCh === b.outCh && a.outMsg === b.outMsg && sameRange (a.outD1, b.outD1) && sameRange (a.outD2, b.outD2)
      && a.outD1Pull === b.outD1Pull && a.outD2Pull === b.outD2Pull;
}

/** The rule as Basic sees it, or null if Basic can't express it exactly. */
export function fromAdvanced (r: MorpherRule): BasicRule | null {
  const single = (rg: Range) => rg[0] === rg[1] && rg[1] >= 0;
  let inNum: number | null = null;
  if (hasNumber (r.inMsg) && single (r.inD1)) inNum = r.inD1[0];
  else if (! isFull (r.inD1)) return null;

  const outType = r.outMsg === 0 ? r.inMsg : r.outMsg;
  const outV = valueSlot (outType);
  const vmax = valueMax (outType);
  const vr: Range | null = outV === 1 ? r.outD1 : outV === 2 ? r.outD2 : null;

  let outNum: number | null = null;
  if (outV !== 1 && hasNumber (outType) && single (r.outD1)) outNum = r.outD1[0];

  let value: ValueChoice = 'same', fixed = 127, limLo = 0, limHi = 127;   // defaults, as DEFAULT_BASIC
  if (vr && ! isFull (vr)) {
    if (vr[0] === vr[1])                  { value = 'fixed'; fixed = vr[0]; }
    else if (vr[0] === vmax && vr[1] === 0) value = 'inverted';
    else                                  { value = 'limited'; limLo = vr[0]; limHi = rHi (vr, vmax); }
  }

  const b: BasicRule = { inKind: r.inMsg, inNum, inCh: r.inCh, outKind: r.outMsg, outNum, outCh: r.outCh,
                         value, fixed, limLo, limHi };
  return sameRule (toAdvanced (b, r), r) ? normaliseBasic (b) : null;
}

// ── The sentence (Basic body, folded header) ─────────────────────────────────
export function basicSentence (b: BasicRule, c: NoteNames): { inText: string; outText: string } {
  const outType = outKindOf (b);
  let inText = b.inKind === 0 ? 'Any event'
             : b.inNum === null && hasNumber (b.inKind) ? `Any ${kindShort (b.inKind)}`
             : `${kindShort (b.inKind)}${b.inNum !== null ? ' ' + numberText (b.inKind, b.inNum, c) : ''}`;
  if (b.inCh) inText += ` on ch ${b.inCh}`;

  const parts: string[] = [];
  if (b.outKind !== 0 || b.outNum !== null)
    parts.push (`${kindShort (outType)}${b.outNum !== null ? ' ' + numberText (outType, b.outNum, c) : ''}`);
  if (b.outCh) parts.push (`${parts.length ? 'on ' : ''}ch ${b.outCh}`);

  const kindChanged = b.inKind !== 0 && outType !== b.inKind;
  let valueText = '';
  if (valueSlot (outType) !== 0) {
    const src = valueSourceName (b.inKind), dst = valueOutName (outType);
    if (b.value === 'same' && kindChanged)  valueText = `${dst} = ${src}`;
    else if (b.value === 'inverted')         valueText = kindChanged ? `${dst} = ${src} inverted` : `${dst} inverted`;
    else if (b.value === 'fixed')            valueText = `${dst} ${b.fixed}`;
    else if (b.value === 'limited')          valueText = `${dst} ${b.limLo}–${b.limHi}`;
  }

  let outText = parts.join (' ');
  if (valueText) outText = outText ? `${outText}, ${valueText}` : valueText;
  return { inText, outText: outText || 'unchanged' };
}
