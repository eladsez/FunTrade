import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig } from 'vitest/config'

// Dev (dashboard/dev.sh): Vite on :5173 proxies /api to the API container.
// Prod: `npm run build` → dist/, served by FastAPI itself.
// Tests (`npm test`, also run by the image build): Vitest in jsdom, src/**/*.test.ts(x).
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: '0.0.0.0',
    // keep the browser's Host: the API refuses writes whose Origin doesn't match it
    proxy: { '/api': { target: process.env.API_URL ?? 'http://localhost:8080', changeOrigin: false } },
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['src/test/setup.ts'],
    // The bot and the owner live in different zones; tests pin the machine's to neither.
    env: { TZ: 'UTC' },
    coverage: { include: ['src/**/*.{ts,tsx}'], exclude: ['src/**/*.test.*', 'src/test/**', 'src/main.tsx'] },
  },
})
