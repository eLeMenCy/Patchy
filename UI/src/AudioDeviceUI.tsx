/**
 * AudioDeviceUI.tsx
 *
 * Settings panel + summary components for Audio In/Out device nodes.
 * Extracted from GenericNode.tsx (2026-07-19) as part of splitting that
 * file into one file per protocol — see Architecture.md's "Next session
 * plan" note (logged 2026-07-15) for the full rationale.
 *
 * Named "*DeviceUI.tsx" not "*NodeUI.tsx" deliberately — mirrors the
 * backend's own AudioDeviceNodes.h/.cpp naming, and avoids colliding with
 * the existing convention where {Protocol}MonitorNode.tsx/
 * {Protocol}ConsoleNode.tsx means "a standalone, independently-registered
 * ReactFlow node component". These are internal helpers GenericNode.tsx
 * imports, not their own registered node type.
 */

import { useState, useEffect, useContext } from 'react';
import { Bridge, type StartupFadeDevices } from './Bridge';
import { Checkbox, SettingsPanelHeader } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import { DawContext } from './DawContext';
import { HintContext } from './HintPanel';

// ── Device selector combobox (unified for MIDI and Audio) ────────────────────
export function DeviceSelector ({ nodeId, nodeType, selectedDeviceId }: {
  nodeId:           string;
  nodeType:         1 | 2 | 3 | 4;
  selectedDeviceId?: string;
}) {
  const isMidi = nodeType <= 2;
  const accent = isMidi ? 'var(--midi)' : 'var(--audio)';
  const { isStandalone, dawLoopbackEnabled, dawHostEnabled } = useContext(DawContext);
  const { setHint } = useContext(HintContext);
  const [devices, setDevices] = useState<Array<{ id: string; name: string }>>([]);
  const [claimed, setClaimed] = useState<Map<string, { deviceId: string; nodeType: number }>>(new Map());

  useEffect(() => {
    const unsubClaimed = Bridge.onClaimedDevices(map => setClaimed(new Map(map)));
    const unsubDevices = isMidi
      ? Bridge.onMidiDevices(list => setDevices(nodeType === 2 ? list.midiOutDevices : list.midiInDevices))
      : Bridge.onAudioDevices(list => setDevices(nodeType === 4 ? list.audioOutDevices : list.audioInDevices));
    return () => { unsubDevices(); unsubClaimed(); };
  }, [nodeType, isMidi]);

  // Grey out devices claimed by nodes of the same direction
  const takenByOthers = new Set<string>();
  claimed.forEach((claim, nId) => {
    if (nId !== nodeId && claim.nodeType === nodeType && claim.deviceId)
      takenByOthers.add(claim.deviceId);
  });

  const isDawLocked = (d: { id: string; dawHost?: boolean }) =>
    d.id === 'DAW' && nodeType === 4 && !isStandalone && !dawLoopbackEnabled;
  const dawHint = { title: 'DAW Loopback Locked 🔒', body: 'Routing audio back to the DAW track risks a feedback loop.\nEnable "DAW loopback" in Preferences → Graph to unlock.' };
  const isDawHostLocked = (d: { id: string; dawHost?: boolean }) => !!d.dawHost && !isStandalone && !dawHostEnabled;
  const dawHostHint = { title: 'DAW Host Device ⚠', body: 'This is your DAW\'s own virtual audio device.\nUsing it may cause signal doubling or unexpected behaviour.\nUse the "DAW" option instead for proper routing.' };
  const opts = devices.map(d => ({
    id:       d.id,
    name:     isDawLocked(d) ? 'DAW  🔒' : isDawHostLocked(d) ? `${d.name}  🔒` : d.name,
    disabled: takenByOthers.has(d.id) || isDawLocked(d) || isDawHostLocked(d),
    warning:  isDawLocked(d) || isDawHostLocked(d),
    hint:     isDawLocked(d) ? dawHint : isDawHostLocked(d) ? dawHostHint : undefined,
  }));
  const paramKey = isMidi ? 'midiDeviceId' : 'audioDeviceId';

  return (
    <NodeSelect
      value={selectedDeviceId ?? ''}
      onChange={v => Bridge.setNodeParam(nodeId, paramKey, v, nodeType)}
      options={opts}
      disabled={devices.length === 0}
      accent={accent}
      onOptionHover={h => setHint(h)}
    />
  );
}

// ── Audio device channel settings panel ───────────────────────────────────────
export function AudioDeviceSettingsPanel ({ nodeId, nodeType, selectedChannels, deviceChannelCount, selectedDeviceId, warning, onClose }: {
  nodeId:             string;
  nodeType:           3 | 4;
  selectedChannels:   number[];
  deviceChannelCount: number;
  selectedDeviceId:   string | undefined;
  warning:            boolean;
  onClose:            () => void;
}) {
  const accent = 'var(--audio)';

  // ── Targeted startup fade (2026-09-24), Audio IN only ──────────────────────
  // No per-node state: the toggle is derived from whether this node's
  // selected DEVICE is in the app-wide list (backend: StartupFadeRegistry.h).
  // Ticking it lists the device with a mute duration; every node on that
  // device, in every graph, shares it from the device's next fresh open.
  const FADE_DEFAULT_MS = 1250;
  const FADE_MAX_MS     = 5000;
  const [fadeDevices, setFadeDevices] = useState<StartupFadeDevices>({});
  useEffect(() => Bridge.onStartupFadeDevices(setFadeDevices), []);
  const fadeMs  = selectedDeviceId ? fadeDevices[selectedDeviceId] : undefined;
  const fadeOn  = fadeMs !== undefined;
  const [msDraft, setMsDraft] = useState<string>(String(FADE_DEFAULT_MS));
  useEffect(() => { setMsDraft(String(fadeOn ? fadeMs : FADE_DEFAULT_MS)); }, [fadeOn, fadeMs]);
  const showFade = nodeType === 3 && !!selectedDeviceId && selectedDeviceId !== 'DAW' && deviceChannelCount > 0;

  const toggleFade = () =>
    Bridge.setAudioInStartupFade(nodeId, !fadeOn, fadeOn ? 0 : FADE_DEFAULT_MS);

  const commitFadeMs = () => {
    const parsed = parseInt(msDraft, 10);
    const v = Number.isFinite(parsed) ? Math.min(FADE_MAX_MS, Math.max(0, parsed)) : (fadeMs ?? FADE_DEFAULT_MS);
    setMsDraft(String(v));
    if (fadeOn && v !== fadeMs) Bridge.setAudioInStartupFade(nodeId, true, v);
  };

  const toggle = (ch: number) => {
    const next = selectedChannels.includes(ch)
      ? selectedChannels.filter(c => c !== ch)
      : [...selectedChannels, ch].sort((a, b) => a - b);
    // Always keep at least one channel selected
    if (next.length === 0) return;
    Bridge.setNodeParam(nodeId, 'audioDeviceChannels', JSON.stringify(next), nodeType);
  };

  const isOut = nodeType === 4;
  const title = isOut ? 'Audio OUT Channels' : 'Audio IN Channels';

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6,
        width: deviceChannelCount > 32 ? 320 : deviceChannelCount > 16 ? 260 : 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={title}
        onReset={() => Bridge.setNodeParam(nodeId, 'audioDeviceChannels', JSON.stringify([0, 1]), nodeType)}
        onClose={onClose}
      />
      {warning && (
        <div style={{
          fontSize: 9, color: '#ef5350', background: 'rgba(239,83,80,0.1)',
          border: '1px solid rgba(239,83,80,0.3)', borderRadius: 3,
          padding: '4px 6px', marginBottom: 6,
        }}>
          Channel selection reset — previous channels not available on this device.
        </div>
      )}
      {deviceChannelCount <= 0 || !selectedDeviceId ? (
        <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 8 }}>
          No device selected
        </div>
      ) : (
        <>
          <div style={{
            fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
            textTransform: 'uppercase', marginTop: 8, marginBottom: 6,
          }}>
            Select channels
          </div>
          <div style={{
            display: 'grid',
            gridTemplateColumns: deviceChannelCount > 32 ? '1fr 1fr 1fr 1fr'
                                : deviceChannelCount > 16 ? '1fr 1fr 1fr'
                                : '1fr 1fr',
            gap: 4,
          }}>
            {Array.from({ length: deviceChannelCount }, (_, i) => (
              <label key={i} style={{
                display: 'flex', alignItems: 'center', gap: 5,
                fontSize: 10, color: 'var(--text-dim)', cursor: 'pointer',
              }}>
                <Checkbox
                  checked={selectedChannels.includes(i)}
                  onChange={() => toggle(i)}
                  accent={accent}
                />
                Ch {i + 1}
              </label>
            ))}
          </div>
          {selectedChannels.length === 0 && (
            <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
              At least one channel required
            </div>
          )}

          {showFade && (
            <div style={{ marginTop: 10, paddingTop: 8, borderTop: '1px solid var(--border)' }}>
              <label style={{
                display: 'flex', alignItems: 'center', gap: 5,
                fontSize: 10, color: 'var(--text-dim)', cursor: 'pointer',
              }}>
                <Checkbox checked={fadeOn} onChange={toggleFade} accent={accent} />
                Startup fade
              </label>
              {fadeOn && (
                <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 6, marginLeft: 18 }}>
                  <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>Mute</span>
                  <input
                    type="number" min={0} max={FADE_MAX_MS} step={50}
                    value={msDraft}
                    onChange={e => setMsDraft(e.target.value)}
                    onKeyDown={e => { if (e.key === 'Enter') { e.preventDefault(); (e.target as HTMLInputElement).blur(); } }}
                    onBlur={commitFadeMs}
                    style={{
                      width: 64, fontSize: 10, padding: '3px 6px',
                      background: 'var(--surface)', border: '1px solid var(--border)',
                      borderRadius: 3, color: 'var(--text-dim)',
                      fontFamily: "'JetBrains Mono', monospace",
                    }}
                  />
                  <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>ms</span>
                </div>
              )}
              <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 6, lineHeight: 1.4 }}>
                {fadeOn
                  ? 'Remembered for this device in every graph. Applies from its next open.'
                  : 'Silences this device briefly after it opens, to hide a startup pop.'}
              </div>
            </div>
          )}
        </>
      )}
    </div>
  );
}

// ── Channel summary label ─────────────────────────────────────────────────────
export function ChannelSummary ({ channels }: { channels: number[] }) {
  if (channels.length === 0) return null;
  const label = channels.map(c => `Ch ${c + 1}`).join(', ');
  return (
    <div style={{
      fontSize: 9, color: 'var(--text-muted)', textAlign: 'center',
      marginBottom: 3, letterSpacing: '0.05em',
    }}>
      {label}
    </div>
  );
}

