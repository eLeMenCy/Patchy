// Patchy — generic MIDI Learn (v0.0.929)
//
// Any node can offer "Learn": arm it, the next channel event arriving at its
// MIDI input is captured by the engine (NodeProcessor::captureLearn, which
// skips Note Offs so a key press learns its Note On) and pushed back in the
// activity feed as `learned`. The node holds its MIDI input back while armed.
// First user: the MIDI Morpher (When/Send in Basic, IN/OUT in Advanced).
//
//   const learn = useMidiLearn<'in' | 'out'> (nodeId, (target, ev) => { … });
//   learn.start ('in')   // click again (same target) to cancel
//   learn.target         // what is armed now, or null
// Gives up after `timeoutMs` (10 s) with nothing received.

import { useCallback, useEffect, useRef, useState } from 'react';
import { Bridge } from './Bridge';

export interface LearnedEvent {
  type: number;   // Morpher codes: 1 Note Off, 2 Note On, 3 Poly AT, 4 CC, 5 Program, 6 Channel AT, 7 Pitch
  ch:   number;   // 1-16
  d1:   number;   // note / CC number / program / pressure; pitch bend: the 14-bit value (0-16383)
  d2:   number;   // velocity / CC value / poly pressure; 0 when the message has no Data2
}

/** Decode NodeProcessor's packing: status | d1 << 8 | d2 << 15 | valid << 63 (decimal string). */
export function decodeLearned (packed: string): LearnedEvent | null {
  let w: bigint;
  try { w = BigInt (packed); } catch { return null; }
  if (((w >> 63n) & 1n) === 0n) return null;
  const status = Number (w & 0xFFn), b1 = Number ((w >> 8n) & 0x7Fn), b2 = Number ((w >> 15n) & 0x7Fn);
  const type = (status >> 4) - 7;   // 0x8_ → 1 … 0xE_ → 7
  if (type < 1 || type > 7) return null;
  const ch = (status & 0x0F) + 1;
  if (type === 7) return { type, ch, d1: b1 | (b2 << 7), d2: 0 };
  if (type === 5 || type === 6) return { type, ch, d1: b1, d2: 0 };
  return { type, ch, d1: b1, d2: b2 };
}

export function useMidiLearn<T extends string> (nodeId: string,
                                                 onLearned: (target: T, ev: LearnedEvent) => void,
                                                 timeoutMs = 10000) {
  const [target, setTarget] = useState<T | null> (null);
  const targetRef = useRef<T | null> (null);
  const cb        = useRef (onLearned);
  cb.current = onLearned;   // always the latest (it closes over the current rule)
  const timer = useRef<number | null> (null);

  const clearTimer = () => { if (timer.current !== null) { clearTimeout (timer.current); timer.current = null; } };

  const stop = useCallback (() => {
    clearTimer();
    if (targetRef.current !== null) Bridge.setNodeParam (nodeId, 'learnArm', '0');
    targetRef.current = null;
    setTarget (null);
  }, [nodeId]);

  const start = useCallback ((t: T) => {
    if (targetRef.current === t) { stop(); return; }   // same button again = cancel
    targetRef.current = t;
    setTarget (t);
    Bridge.setNodeParam (nodeId, 'learnArm', '1');
    clearTimer();
    timer.current = window.setTimeout (stop, timeoutMs);
  }, [nodeId, stop, timeoutMs]);

  useEffect (() => Bridge.onPortActivity (entries => {
    const e = entries.find (x => x.id === nodeId);
    if (! e?.learned || targetRef.current === null) return;
    const t = targetRef.current;
    clearTimer();
    targetRef.current = null;
    setTarget (null);
    const ev = decodeLearned (e.learned);
    if (ev) cb.current (t, ev);
  }), [nodeId]);

  // Leaving (node deleted, unmounted) while armed: disarm the engine.
  useEffect (() => () => {
    clearTimer();
    if (targetRef.current !== null) Bridge.setNodeParam (nodeId, 'learnArm', '0');
  }, [nodeId]);

  return { target, start, stop };
}
