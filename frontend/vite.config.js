import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: {
      '/agents': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true,
        ws: true
      },
      '/memory': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true
      },
      '/plugins': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true
      },
      '/models': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true
      },
      '/healthz': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true
      },
      '/workspace': {
        target: 'http://127.0.0.1:8000',
        changeOrigin: true
      }
    }
  }
})
