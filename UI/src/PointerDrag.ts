// PointerDrag.ts — v0.0.978: dragging from the sidebar on Linux.
//
// On Linux JUCE runs WebKitGTK in a separate process whose window is plugged
// into Patchy's (XEmbed): an HTML5 drag starts (the ghost image follows the
// mouse) but the drop never reaches the page (user, Garuda, 2026-11-09).
// There the sidebar items are dragged with plain pointer events instead: a
// small label follows the mouse, and on release a `patchy-pointer-drop`
// window event carries the same payload as the HTML5 drag's 'text/plain'
// data plus the release point; App drops it exactly like an HTML5 drop.
// macOS and Windows keep the native drag.

export const POINTER_DROP_EVENT = 'patchy-pointer-drop';

export interface PointerDropDetail { raw: string; x: number; y: number }

/** True where the native HTML5 drag-and-drop can't be trusted (Linux / WebKitGTK). */
export const usePointerDrag =
  typeof navigator !== 'undefined' && /Linux/.test(navigator.userAgent) && !/Android/i.test(navigator.userAgent);

/** Call from an item's onPointerDown. `payload` = what the HTML5 drag puts in 'text/plain'. */
export function startPointerDrag(e: { button: number; clientX: number; clientY: number }, payload: string, label: string, accent = 'var(--accent)') {
  if (!usePointerDrag || e.button !== 0) return;
  const x0 = e.clientX, y0 = e.clientY;
  let ghost: HTMLDivElement | null = null;
  const prevSelect = document.body.style.userSelect;

  const move = (ev: PointerEvent) => {
    if (!ghost) {
      if (Math.hypot(ev.clientX - x0, ev.clientY - y0) < 5) return;   // a click, not a drag (yet)
      ghost = document.createElement('div');
      ghost.textContent = label;
      Object.assign(ghost.style, {
        position: 'fixed', zIndex: '1000', pointerEvents: 'none',
        padding: '4px 10px', borderRadius: '6px', fontSize: '11px',
        fontFamily: "'JetBrains Mono', monospace", color: 'var(--text)',
        background: 'var(--menu-bg, #1a1e2d)', border: `1px solid ${accent}`,
        boxShadow: '0 6px 20px rgba(0,0,0,.5)', opacity: '0.9',
        transform: 'translate(8px, 8px)',
      } as Partial<CSSStyleDeclaration>);
      document.body.appendChild(ghost);
      document.body.style.userSelect = 'none';
    }
    ghost.style.left = ev.clientX + 'px';
    ghost.style.top  = ev.clientY + 'px';
  };
  const end = (ev: PointerEvent, drop: boolean) => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
    window.removeEventListener('pointercancel', cancel);
    window.removeEventListener('keydown', esc);
    document.body.style.userSelect = prevSelect;
    if (!ghost) return;   // never moved: it was a click
    ghost.remove();
    if (drop)
      window.dispatchEvent(new CustomEvent<PointerDropDetail> (POINTER_DROP_EVENT, { detail: { raw: payload, x: ev.clientX, y: ev.clientY } }));
  };
  const up     = (ev: PointerEvent) => end (ev, true);
  const cancel = (ev: PointerEvent) => end (ev, false);
  const esc    = (ev: KeyboardEvent) => { if (ev.key === 'Escape') end (new PointerEvent ('pointercancel'), false); };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
  window.addEventListener('pointercancel', cancel);
  window.addEventListener('keydown', esc);
}
