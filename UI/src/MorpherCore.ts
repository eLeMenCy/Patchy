// Patchy — MIDI Morpher: pure rule logic shared by the node UI (v0.0.927)
//
// No React here, so it can be run and checked on its own. Mirrors the C++
// engine (Source/MidiMorpherNode.h) — keep the two in sync.

export type Range = [number, number];   // [lo, hi], hi -1 = Max

export interface MorpherRule {
  inCh: number; inMsg: number; inD1: Range; inD2: Range;
  outCh: number; outMsg: number; outD1: Range; outD2: Range;
  outD1Pull: boolean; outD2Pull: boolean;
  unmatched: 'pass' | 'block';
  // UI only (the engine ignores them) — v0.0.927
  mode:      'basic' | 'advanced';
  noteNames: 'yamaha' | 'roland';   // middle C (60) = C3 / C4, as in the MIDI Monitor
}

export const DEFAULT_RULE: MorpherRule = {
  inCh: 0, inMsg: 0, inD1: [0, -1], inD2: [0, -1],
  outCh: 0, outMsg: 0, outD1: [0, -1], outD2: [0, -1],
  outD1Pull: false, outD2Pull: false,
  unmatched: 'pass',
  mode: 'basic', noteNames: 'yamaha',
};

// Short names match MidiDash's menu (node body + Advanced panel).
export const MSG_NAMES = ['Any', 'NoteOff', 'NoteOn', 'Afttouch', 'CC', 'Program', 'ChannelAT', 'Pitch'];
export const PITCH = 7;
export const hasData2 = (m: number) => m >= 1 && m <= 4;
export const data1Max = (m: number) => (m === PITCH ? 16383 : 127);

export function parseRange (v: unknown, fallback: Range): Range {
  return Array.isArray (v) && v.length === 2 && typeof v[0] === 'number' && typeof v[1] === 'number'
    ? [v[0], v[1]] : fallback;
}

export function parseRule (json: string | undefined): MorpherRule {
  if (! json) return { ...DEFAULT_RULE };
  try {
    const p = JSON.parse (json);
    const num = (k: string, d: number) => (typeof p[k] === 'number' ? p[k] : d);
    return {
      inCh:  num ('inCh', 0),  inMsg:  num ('inMsg', 0),
      inD1:  parseRange (p.inD1, [0, -1]),  inD2:  parseRange (p.inD2, [0, -1]),
      outCh: num ('outCh', 0), outMsg: num ('outMsg', 0),
      outD1: parseRange (p.outD1, [0, -1]), outD2: parseRange (p.outD2, [0, -1]),
      outD1Pull: !! p.outD1Pull, outD2Pull: !! p.outD2Pull,
      unmatched: p.unmatched === 'block' ? 'block' : 'pass',
      mode:      p.mode === 'advanced' ? 'advanced' : 'basic',
      noteNames: p.noteNames === 'roland' ? 'roland' : 'yamaha',
    };
  } catch { return { ...DEFAULT_RULE }; }
}

/** Keep explicit values inside a slot's range after a message-type change. */
export function clampRange ([lo, hi]: Range, max: number): Range {
  const l = Math.min (lo, max);
  const h = hi < 0 || hi >= max ? -1 : hi;
  return [l, h];
}

/** Re-clamp every data range to the current message types. */
export function normalise (r: MorpherRule): MorpherRule {
  const outType = r.outMsg === 0 ? r.inMsg : r.outMsg;
  return {
    ...r,
    inD1:  clampRange (r.inD1,  data1Max (r.inMsg)),
    inD2:  clampRange (r.inD2,  127),
    outD1: clampRange (r.outD1, data1Max (outType)),
    outD2: clampRange (r.outD2, 127),
  };
}

export const fmtLo = (v: number) => (v === 0 ? 'Min' : String (v));
export const fmtHi = (v: number) => (v < 0 ? 'Max' : String (v));

// ── Engine mirror (same maths as Source/MidiMorpherNode.h) ──────────────────
export const isFull = (rg: Range) => rg[0] === 0 && rg[1] < 0;
export const rLo = (rg: Range, max: number) => Math.min (rg[0], max);
export const rHi = (rg: Range, max: number) => (rg[1] < 0 ? max : Math.min (rg[1], max));
export const clampTo = (v: number, max: number) => Math.max (0, Math.min (max, v));

export function scaleValue (has: boolean, v: number, inRg: Range, inMax: number, outRg: Range, outMax: number): number {
  if (isFull (outRg)) {
    if (! has) return 0;
    return inMax === outMax ? clampTo (v, outMax) : clampTo (Math.round (v * outMax / inMax), outMax);
  }
  const inLo = rLo (inRg, inMax), inHi = rHi (inRg, inMax);
  const outLo = rLo (outRg, outMax), outHi = rHi (outRg, outMax);
  if (! has || inLo === inHi) return outLo;
  return clampTo (Math.round (outLo + (v - inLo) / (inHi - inLo) * (outHi - outLo)), outMax);
}

/** Where an OUT data slot takes its value from (Pull aware). */
export interface SlotMap {
  srcName: string; srcHas: boolean; srcRange: Range; srcMax: number;
  outRange: Range; outMax: number; exists: boolean;
}
export function slotMap (r: MorpherRule, slot: 1 | 2): SlotMap {
  const outType = r.outMsg === 0 ? r.inMsg : r.outMsg;
  const fromD2  = slot === 1 ? r.outD1Pull : ! r.outD2Pull;
  const src = fromD2
    ? { srcName: 'IN Data2', srcHas: r.inMsg === 0 || hasData2 (r.inMsg), srcRange: r.inD2, srcMax: 127 }
    : { srcName: 'IN Data1', srcHas: true, srcRange: r.inD1, srcMax: data1Max (r.inMsg) };
  return slot === 1
    ? { ...src, outRange: r.outD1, outMax: data1Max (outType), exists: true }
    : { ...src, outRange: r.outD2, outMax: 127, exists: outType === 0 || hasData2 (outType) };
}

// ── Rule summary (folded header) — a first taste of Basic's sentence ────────
export function ruleSummary (r: MorpherRule): string {
  const val = (rg: Range) => (isFull (rg) ? '' : rg[0] === rg[1] ? ` ${rg[0]}` : ` ${fmtLo (rg[0])}–${fmtHi (rg[1])}`);
  const inD2 = isFull (r.inD2) || (r.inMsg !== 0 && ! hasData2 (r.inMsg)) ? '' : ` d2${val (r.inD2)}`;
  const inS  = `${r.inMsg ? MSG_NAMES[r.inMsg] : 'All'}${val (r.inD1)}${inD2}${r.inCh ? ` ch${r.inCh}` : ''}`;
  const name = r.outMsg ? MSG_NAMES[r.outMsg] : (r.inMsg ? MSG_NAMES[r.inMsg] : '');
  const d2   = isFull (r.outD2) && ! r.outD2Pull ? '' : ` d2${r.outD2Pull ? '←D1' : ''}${val (r.outD2)}`;
  const changed = r.outMsg !== 0 || r.outCh !== 0 || r.outD1Pull || r.outD2Pull || ! isFull (r.outD1) || ! isFull (r.outD2);
  const outS = changed
    ? `${name}${r.outD1Pull ? ' ←D2' : ''}${val (r.outD1)}${d2}${r.outCh ? ` ch${r.outCh}` : ''}`.trim()
    : 'same';
  return `${inS} → ${outS}`;
}

// ── Warnings: parts of a rule that can't do what they say ──────────────────
export interface RuleNote { level: 'warn' | 'info'; text: string }
export function ruleNotes (r: MorpherRule): RuleNote[] {
  const notes: RuleNote[] = [];
  const outType = r.outMsg === 0 ? r.inMsg : r.outMsg;
  const inNo2  = r.inMsg !== 0 && ! hasData2 (r.inMsg);
  const outNo2 = outType !== 0 && ! hasData2 (outType);
  const inName = MSG_NAMES[r.inMsg], outName = MSG_NAMES[outType];
  if (inNo2 && ! isFull (r.inD2))
    notes.push ({ level: 'warn', text: `IN Data2 range is ignored: ${inName} has no Data2.` });
  if (outNo2 && (! isFull (r.outD2) || r.outD2Pull))
    notes.push ({ level: 'warn', text: `OUT Data2 is ignored: ${outName} has no Data2.` });
  if (inNo2 && r.outD1Pull)
    notes.push ({ level: 'warn', text: `OUT Data1 pulls Data2, which ${inName} doesn't have: OUT Data1 is always ${rLo (r.outD1, data1Max (outType))}.` });
  if (inNo2 && ! outNo2 && ! r.outD2Pull)
    notes.push ({ level: 'warn', text: `${inName} has no Data2: OUT Data2 is always ${rLo (r.outD2, 127)}.` });
  const changesNothing = r.outCh === 0 && (r.outMsg === 0 || r.outMsg === r.inMsg)
    && isFull (r.outD1) && isFull (r.outD2) && ! r.outD1Pull && ! r.outD2Pull;
  if (changesNothing && r.unmatched === 'pass')
    notes.push ({ level: 'info', text: 'The rule changes nothing yet: every event goes out unchanged.' });
  else if (changesNothing)
    notes.push ({ level: 'info', text: 'Filter only: matching events go out unchanged, the rest is blocked.' });
  return notes;
}

// ── Live feedback decoding (MidiMorpherNode::packMorph) ─────────────────────
export interface MorphSide { t: number; ch: number; d1: number; d2: number }
export function decodeMorph (packed: string): { in: MorphSide; out: MorphSide } | null {
  let w: bigint;
  try { w = BigInt (packed); } catch { return null; }
  if (((w >> 63n) & 1n) === 0n) return null;
  const side = (x: bigint): MorphSide => ({
    t: Number (x & 7n), ch: Number ((x >> 3n) & 15n) + 1, d1: Number ((x >> 7n) & 0x3FFFn), d2: Number ((x >> 21n) & 0x7Fn),
  });
  return { in: side (w & 0xFFFFFFFn), out: side ((w >> 28n) & 0xFFFFFFFn) };
}
export function eventText (e: MorphSide): string {
  const name = MSG_NAMES[e.t] ?? '?';
  if (e.t === 1 || e.t === 2) return `ch${e.ch} ${name} ${e.d1} v${e.d2}`;
  if (e.t === 3 || e.t === 4) return `ch${e.ch} ${name} ${e.d1} = ${e.d2}`;
  return `ch${e.ch} ${name} ${e.d1}`;
}
