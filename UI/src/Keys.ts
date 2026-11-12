// Keys.ts — v0.0.982: platform-aware shortcuts.
//
// The UI only listened for ⌘ (metaKey): on Linux / Windows that's the
// Super / Windows key, and every ⌘ shortcut was dead there (user, Garuda,
// 2026-11-12). Elsewhere the convention is Ctrl: cmdDown() reads ⌘ on the
// Mac, Ctrl elsewhere, and shortcutLabel() turns "⌥⌘L" into "Ctrl+Alt+L".
//
// On the Mac some shortcuts (⌘N / O / S / Q / . / Z / G) are caught by C++
// (PatchyEditor::keyPressed). On Linux and Windows the keystrokes go to the
// web view and C++ never sees them: there the page handles them all
// (App, "non-Mac shortcuts"), and C++'s handling is Mac-only.

export const isMac = typeof navigator !== 'undefined' &&
  /Mac|iPhone|iPad/.test((navigator as Navigator & { userAgentData?: { platform?: string } }).userAgentData?.platform
                         ?? navigator.platform ?? navigator.userAgent);

/** ⌘ on the Mac, Ctrl elsewhere. */
export const cmdDown = (e: { metaKey: boolean; ctrlKey: boolean }) => isMac ? e.metaKey : e.ctrlKey;

/** The key name of that modifier, as KeyboardEvent.key reports it. */
export const CMD_KEY_NAME = isMac ? 'Meta' : 'Control';

/** "⌘⇧S" → "⌘⇧S" on the Mac, "Ctrl+Shift+S" elsewhere (Ctrl, Alt, Shift order). */
export function shortcutLabel (mac: string): string {
  if (isMac) return mac;
  let ctrl = false, alt = false, shift = false, rest = '';
  for (const ch of mac) {
    if      (ch === '⌘') ctrl  = true;
    else if (ch === '⌥') alt   = true;
    else if (ch === '⇧') shift = true;
    else rest += ch;
  }
  return [ctrl && 'Ctrl', alt && 'Alt', shift && 'Shift', rest].filter(Boolean).join('+');
}
