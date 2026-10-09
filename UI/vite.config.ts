import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  base: './',   // relative paths so binary-data embedding works
  build: {
    outDir: 'dist',
    // One bundle on purpose (embedded in the binary, never downloaded), so
    // Vite's 500 kB web-download warning doesn't apply (v0.0.929).
    chunkSizeWarningLimit: 2000,
    rollupOptions: {
      output: {
        // Single JS and CSS chunk for easy JUCE binary-data embedding
        entryFileNames: 'assets/index.js',
        chunkFileNames:  'assets/index.js',
        assetFileNames:  'assets/[name][extname]',
      },
    },
  },
})
