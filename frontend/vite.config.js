import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [vue(), tailwindcss()],
  build: {
    outDir: '../spiffs_data',
    emptyOutDir: true,
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        chunkFileNames: 'app.js',
        assetFileNames: (info) => {
          if (info.name && info.name.endsWith('.css')) return 'app.css'
          return '[name][extname]'
        }
      }
    }
  },
  server: {
    proxy: {
      '/api': 'http://192.168.1.1:80',
      '/stream': 'http://192.168.1.1:81',
      '/audio': 'http://192.168.1.1:82'
    }
  }
})
