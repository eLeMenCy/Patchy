/**
 * Perf.ts — v0.0.994: lightweight counters for the performance overlay
 *
 * The user's high-load graph made the UI stutter (audio / MIDI fine). Before
 * changing anything we measure: what reaches the UI, how often, and how long
 * our own JS spends on it. Each probe is two numbers per name and per
 * second — a count and the milliseconds spent — so it costs next to nothing,
 * and nothing at all while the overlay is off (`perfOn` false: early return).
 *
 *   perfCount ('flash css')            → one more event
 *   const t = perfStart (); …; perfEnd ('activity', t)   → count + time spent
 */

export let perfOn = false;
export function setPerfOn (on: boolean) { perfOn = on; if (! on) reset (); }

interface Acc { n: number; ms: number }
let cur  = new Map<string, Acc> ();
let last = new Map<string, Acc> ();
let since = performance.now ();

function reset () { cur = new Map (); last = new Map (); since = performance.now (); }

function roll () {
  const now = performance.now ();
  if (now - since >= 1000) { last = cur; cur = new Map (); since = now; }
}

export function perfCount (name: string, n = 1) {
  if (! perfOn) return;
  roll ();
  const a = cur.get (name) ?? { n: 0, ms: 0 };
  a.n += n; cur.set (name, a);
}

export function perfStart (): number { return perfOn ? performance.now () : 0; }

export function perfEnd (name: string, t0: number) {
  if (! perfOn || t0 === 0) return;
  roll ();
  const a = cur.get (name) ?? { n: 0, ms: 0 };
  a.n += 1; a.ms += performance.now () - t0; cur.set (name, a);
}

/** The last complete second, per probe name (sorted by time spent, then count). */
export function perfSnapshot (): [string, Acc][] {
  roll ();
  return [...last.entries ()].sort ((a, b) => b[1].ms - a[1].ms || b[1].n - a[1].n);
}
