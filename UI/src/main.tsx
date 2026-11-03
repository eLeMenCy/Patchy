import { Component, StrictMode, type ErrorInfo, type ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App'

// v0.0.964 — a render error used to unmount the whole UI (all black, only the
// WebView's Reload brought it back). Now the error is shown, with a Reload.
class CrashScreen extends Component<{ children: ReactNode }, { error: Error | null; stack: string }> {
  state = { error: null as Error | null, stack: '' };
  static getDerivedStateFromError (error: Error) { return { error }; }
  componentDidCatch (error: Error, info: ErrorInfo) {
    console.error ('[Patchy] UI crashed:', error, info.componentStack);
    this.setState ({ stack: info.componentStack ?? '' });
  }
  render() {
    const { error, stack } = this.state;
    if (! error) return this.props.children;
    const text = `${error.name}: ${error.message}\n\n${error.stack ?? ''}\n\nComponents:${stack}`;
    return (
      <div style={{ position: 'fixed', inset: 0, padding: 24, overflow: 'auto', background: 'var(--bg, #111)',
                    color: 'var(--text, #ddd)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11 }}>
        <div style={{ fontSize: 14, fontWeight: 600, marginBottom: 8 }}>Patchy's UI hit an error</div>
        <div style={{ marginBottom: 12, color: 'var(--text-dim, #999)' }}>
          The patch itself keeps running. Copy the text below for the bug report, then reload.
        </div>
        <div style={{ display: 'flex', gap: 8, marginBottom: 12 }}>
          <button onClick={() => location.reload()}>Reload UI</button>
          <button onClick={() => navigator.clipboard?.writeText (text)}>Copy error</button>
        </div>
        <pre style={{ whiteSpace: 'pre-wrap', userSelect: 'text' }}>{text}</pre>
      </div>
    );
  }
}

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <CrashScreen>
      <App />
    </CrashScreen>
  </StrictMode>
)
