import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  base: './',   // relative paths so binary-data embedding works
  build: {
    outDir: 'dist',
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
