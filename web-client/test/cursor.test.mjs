// @vitest-environment happy-dom
import { test, expect } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
import RemoteViewer from '../src/components/RemoteViewer.vue';

test('removes the hidden inline style when capturing again', async () => {
  const cursorStyle = reactive({ left: '100px', top: '100px' });
  const state = reactive({ controlActive: true, fullscreenActive: true });
  const root = document.createElement('div');
  document.body.append(root);
  const app = createApp({
    render: () => h(RemoteViewer, {
      ...state, cursorStyle, videoStyle: {}, frameVisible: true,
      status: '', statusError: false, performanceStats: {},
      touchMode: 'trackpad', activeModifiers: [], orientationLocked: false,
      wakeLockEnabled: false, viewportZoom: 1,
    }),
  });
  app.mount(root);
  try {
    const cursor = root.querySelector('#remote-cursor');
    expect(cursor.style.display).toBe('');
    for (let capture = 0; capture < 3; capture += 1) {
      state.controlActive = false;
      state.fullscreenActive = false;
      cursorStyle.display = 'none';
      await nextTick();
      expect(cursor.style.display).toBe('none');

      state.controlActive = true;
      state.fullscreenActive = true;
      delete cursorStyle.display;
      await nextTick();
      expect(cursor.style.display).toBe('');
      cursorStyle.left = `${120 + capture}px`;
      await nextTick();
      expect(cursor.style.left).toBe(`${120 + capture}px`);
    }
  } finally {
    app.unmount();
    root.remove();
  }
});
