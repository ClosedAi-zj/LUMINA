import type { LuminaPreload } from '../electron/preload';

declare global {
  interface Window {
    lumina: LuminaPreload;
  }
}

export {};
