/**
 * PaxSettings.ts — v0.0.995: a Pax node's saved settings with text parameters
 *
 * A Pax node's settingsJson has always been an array of numbers, one per
 * parameter. Text parameters (PaxAPI.h, v0.0.995) are kept in one trailing
 * entry, {"text": ["/mixer/fader1", …]}, so everything reading the numbers
 * by index keeps working, and a node without text parameters is unchanged.
 * The backend reads the same trailing entry on a rebuild (ProcessingGraph).
 */

import { _paxInfoMap } from './NodeUtils';

/** The texts saved in a settings array (null if none). */
export function textsIn (raw: unknown): string[] | null {
  if (! Array.isArray (raw) || raw.length === 0) return null;
  const last = raw[raw.length - 1] as { text?: unknown } | null;
  return last && typeof last === 'object' && Array.isArray (last.text) ? last.text.map (t => String (t ?? '')) : null;
}

/** The texts of a Pax node: saved, else the Pax's defaults ([] if it has none). */
export function paxTexts (paxName: string | undefined, settingsJson: string | undefined): string[] {
  const info = paxName ? _paxInfoMap.get (paxName)?.textParams ?? [] : [];
  let saved: string[] | null = null;
  try { saved = textsIn (settingsJson ? JSON.parse (settingsJson) : null); } catch { saved = null; }
  if (info.length === 0) return saved ?? [];   // Pax list not in yet: what was saved
  return info.map ((t, k) => saved && typeof saved[k] === 'string' ? saved[k] : t.defaultValue);
}

/** The array to save: the numbers, then the texts (if the Pax has any). */
export function withTexts (vals: number[], texts: string[] | null | undefined): unknown[] {
  return texts && texts.length > 0 ? [...vals, { text: texts }] : vals;
}
