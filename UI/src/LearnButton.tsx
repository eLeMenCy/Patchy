// LearnButton.tsx — v0.0.947: the Morpher's Learn pill, shared by every node
// that offers MIDI Learn (see Learn.ts for the mechanism).
export function LearnButton ({ armed, onClick, accent = 'var(--midi)', what }: {
  armed: boolean; onClick: () => void; accent?: string;
  what?: string;   // what the next event fills in, for the tooltip ("the CC number", "the channel")
}) {
  return (
    <span className="nodrag" onClick={onClick} onDoubleClick={e => e.stopPropagation()}
      title={armed ? 'Listening — move a control or play a key (click to cancel; gives up after 10 s)'
                   : `Learn: move a control or play a key on a connected controller to fill in ${what ?? 'this'}`}
      style={{ fontSize: 9, letterSpacing: 0, textTransform: 'none', cursor: 'pointer', padding: '1px 6px',
               borderRadius: 3, border: `1px solid ${armed ? accent : 'var(--border)'}`, whiteSpace: 'nowrap',
               color: armed ? 'var(--bg)' : 'var(--text-dim)', background: armed ? accent : 'transparent',
               animation: armed ? 'patchyLearnPulse 1s ease-in-out infinite' : 'none' }}>
      {armed ? '● Listening…' : 'Learn'}
    </span>
  );
}
// One keyframe for the pulsing Learn button (injected once)
if (typeof document !== 'undefined' && ! document.getElementById ('patchy-learn-kf')) {
  const st = document.createElement ('style');
  st.id = 'patchy-learn-kf';
  st.textContent = '@keyframes patchyLearnPulse { 0%,100% { opacity: 1 } 50% { opacity: .55 } }';
  document.head.appendChild (st);
}
