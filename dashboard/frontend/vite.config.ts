import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig } from 'vite'

// Dev (dashboard/dev.sh): Vite on :5173 proxies /api to the API container.
// Prod: `npm run build` → dist/, served by FastAPI itself.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: '0.0.0.0',
    // keep the browser's Host: the API refuses writes whose Origin doesn't match it
    proxy: { '/api': { target: process.env.API_URL ?? 'http://localhost:8080', changeOrigin: false } },
  },
})
