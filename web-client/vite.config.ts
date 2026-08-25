import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';

const devApi = process.env.REMOE_DEV_API ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [vue()],
  build: {
    target: 'es2022',
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
