// KbdFaders.ts — v0.0.956: the MIDI Keyboard's faders (user's idea,
// 2026-10-25), shared by the Keyboard node and the wrap's ⚙ panel.
//
// Each fader sends one kind of event on the Keyboard's channel:
//   cc    CC n (0-127), value 0-127
//   pc    program change, value 0-127 (sent when you let go, not while dragging)
//   cc14  14-bit CC n (0-31) + n+32 (LSB), value 0-16383
//   nrpn  NRPN n (0-16383) via CC 99/98, data CC 6/38, 0-16383
// Values are saved in the Keyboard's settings ("faders") and re-sent when a
// project opens or an undo / redo changes them.
import { Bridge } from './Bridge';

export type KbdFaderType = 'cc' | 'pc' | 'cc14' | 'nrpn';   // (no aftertouch: the keyboard's own pressure does it — user, 2026-10-27)
export interface KbdFader { type: KbdFaderType; num: number; value: number }

// Preferences (App sets it): how many faders
let maxKbdFaders = 4;
export const setKbdFaderPrefs = (max: number) => { maxKbdFaders = Math.max (1, Math.min (16, Math.round (max || 4))); };
export const getMaxKbdFaders = () => maxKbdFaders;

export const isWide   = (t: KbdFaderType) => t === 'cc14' || t === 'nrpn';
export const maxValue = (t: KbdFaderType) => isWide (t) ? 16383 : 127;
export const hasNum   = (t: KbdFaderType) => t === 'cc' || t === 'cc14' || t === 'nrpn';
export const maxNum   = (t: KbdFaderType) => t === 'nrpn' ? 16383 : t === 'cc14' ? 31 : 127;

export const TYPE_NAMES: Record<KbdFaderType, string> = {
  cc: 'CC', pc: 'Program', cc14: 'CC 14-bit', nrpn: 'NRPN',
};

export function faderLabel (f: KbdFader): string {
  switch (f.type) {
    case 'cc':   return `CC ${f.num}`;
    case 'cc14': return `CC ${f.num} (14-bit)`;
    case 'nrpn': return `NRPN ${f.num}`;
    case 'pc':   return 'Program';
  }
}

/** The faders in a Keyboard's settings, cleaned up. */
export function fadersOfSettings (s: Record<string, unknown> | undefined): KbdFader[] {
  const a = s?.faders;
  if (! Array.isArray (a)) return [];
  return a.slice (0, 16).map ((x: any): KbdFader => {
    const type: KbdFaderType = ['cc', 'pc', 'cc14', 'nrpn'].includes (x?.type) ? x.type : 'cc';
    const clamp = (v: unknown, hi: number, d: number) => Math.max (0, Math.min (hi, Math.round (Number (v ?? d)) || 0));
    return { type, num: clamp (x?.num, maxNum (type), 1), value: clamp (x?.value, maxValue (type), 0) };
  });
}

/** The channel the Keyboard sends on (Omni → 1). */
export const kbdChannel = (s: Record<string, unknown> | undefined) => {
  const c = Number (s?.channel ?? 1);
  return c >= 1 && c <= 16 ? c : 1;
};

// What each fader last sent, per node — so the Keyboard (re-sending saved
// values after an undo / project open) and the wrap's panel never send the
// same value twice
const lastSent = new Map<string, string> ();
const sentKey  = (nodeId: string, k: number) => `${nodeId}|${k}`;
const sig      = (ch: number, f: KbdFader) => `${ch}|${f.type}|${f.num}|${f.value}`;

/** Send fader k's value. Program change only when `final` (let go / typed). */
export function sendKbdFader (nodeId: string, ch: number, k: number, f: KbdFader, final: boolean) {
  if (f.type === 'pc' && ! final) return;
  const s = sig (ch, f);
  if (lastSent.get (sentKey (nodeId, k)) === s && ! (f.type === 'pc')) return;
  lastSent.set (sentKey (nodeId, k), s);
  const c = (ch - 1) & 0x0F, v = f.value;
  const send = (st: number, d1: number, d2: number) => Bridge.sendMidiKeyEvent (nodeId, st | c, d1 & 0x7F, d2 & 0x7F);
  switch (f.type) {
    case 'cc':   send (0xB0, f.num, v); break;
    case 'pc':   send (0xC0, v, 0); break;
    case 'cc14': send (0xB0, f.num, v >> 7); send (0xB0, f.num + 32, v); break;
    case 'nrpn': send (0xB0, 99, f.num >> 7); send (0xB0, 98, f.num); send (0xB0, 6, v >> 7); send (0xB0, 38, v); break;
  }
}

/** After a restore (undo / redo, project open): send what changed. */
export function resendKbdFaders (nodeId: string, ch: number, faders: KbdFader[]) {
  faders.forEach ((f, k) => {
    if (lastSent.get (sentKey (nodeId, k)) !== sig (ch, f)) sendKbdFader (nodeId, ch, k, f, true);
  });
}
