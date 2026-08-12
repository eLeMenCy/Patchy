// Patchy — DAW Context (shared state for standalone/DAW mode)
import { createContext } from 'react';

export interface DawContextType {
  isStandalone:       boolean;
  dawLoopbackEnabled: boolean;
  setDawLoopback:     (v: boolean) => void;
  dawHostEnabled:     boolean;
  setDawHost:         (v: boolean) => void;
}

export const DawContext = createContext<DawContextType>({
  isStandalone:       true,
  dawLoopbackEnabled: false,
  setDawLoopback:     () => {},
  dawHostEnabled:     false,
  setDawHost:         () => {},
});
