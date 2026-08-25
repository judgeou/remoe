import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'node:path';

const devApi = process.env.REMOE_DEV_API ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [vue()],
  build: {
    target: 'es2022',
    rollupOptions: {
      input: {
        app: resolve(import.meta.dirname, 'index.html'),
        diagnostics: resolve(import.meta.dirname, 'diagnostics.html'),
      },
    },
  },
  server: {
    proxy: {
      '/api': devApi,
      '/healthz': devApi,
      '/signal': { target: devApi, ws: true },
      '/host': { target: devApi, ws: true },
    },
  },
  test: {
    environment: 'node',
  },
});
