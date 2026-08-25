import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  plugins: [vue()],
  build: {
    target: 'es2022',
  },
  test: {
    environment: 'node',
  },
});
