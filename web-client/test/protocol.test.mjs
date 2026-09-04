import { test } from 'vitest';
import assert from 'node:assert/strict';
import {
  MAGIC,
  SIGNAL_TYPE,
  SignalFrameBuffer,
  VideoDecodeGate,
  VideoFrameAssembler,
  decodeStreamHeader,
  decodeStreamStatus,
  decodeClockSyncResponse,
  decodeClipboardText,
  decodeCursorState,
  encodeClientConfig,
  encodeClockSyncRequest,
  encodeInputEvent,
  encodeClipboardText,
  encodeSignal,
  h264CodecString,
} from '../src/core/protocol.js';
import { parseInvite } from '../src/core/remoe-client.js';
import { ClipboardSynchronizer } from '../src/core/clipboard-sync.js';
import { RemoteInputController, windowsScanCode } from '../src/core/input.js';
import { LatestFrameRenderer } from '../src/core/latest-frame-renderer.js';
import { cursorViewportPosition, fitVideoSize } from '../src/core/layout.js';

function testVideoFrame(name) {
  return {
    name,
    closed: 0,
    close() { this.closed += 1; },
  };
}

test('latest-frame renderer draws only the newest frame in one refresh cycle', () => {
  const scheduled = [];
  const rendered = [];
  const presented = [];
  const renderer = new LatestFrameRenderer(
    (frame) => rendered.push(frame.name),
    { schedule: (callback) => { scheduled.push(callback); return scheduled.length; } },
  );
  const first = testVideoFrame('first');
  const second = testVideoFrame('second');

  renderer.submit(first, () => presented.push('first'));
  renderer.submit(second, () => presented.push('second'));

  assert.equal(scheduled.length, 1);
  assert.equal(first.closed, 1);
  assert.equal(second.closed, 0);
  scheduled[0]();
  assert.deepEqual(rendered, ['second']);
  assert.deepEqual(presented, ['second']);
  assert.equal(second.closed, 1);
});

test('latest-frame renderer cancels and releases a pending frame on dispose', () => {
  const canceled = [];
  const renderer = new LatestFrameRenderer(
    () => assert.fail('disposed renderer must not draw'),
    {
      schedule: () => 42,
      cancel: (requestId) => canceled.push(requestId),
    },
  );
  const frame = testVideoFrame('pending');

  renderer.submit(frame);
  renderer.dispose();

  assert.deepEqual(canceled, [42]);
  assert.equal(frame.closed, 1);
});

test('encodes protocol v11 CBR client settings as little-endian packed bytes', () => {
  const bytes = encodeClientConfig({ fps: 90, bitrateMbps: 25, scalePercent: 75 });
  const view = new DataView(bytes.buffer);
  assert.equal(bytes.length, 36);
  assert.equal(view.getUint32(0, true), MAGIC.clientConfig);
  assert.equal(view.getUint16(4, true), 11);
  assert.equal(view.getUint32(8, true), 90);
  assert.equal(view.getUint32(16, true), 25_000_000);
  assert.equal(view.getUint32(20, true), 75);
  assert.equal(view.getUint32(24, true), 15);
  assert.equal(view.getUint32(28, true), 0);
  assert.equal(view.getUint32(32, true), 0);
});

test('encodes fixed-quality AV1 client settings', () => {
  const bytes = encodeClientConfig({
    fps: 60, bitrateMbps: 40, rateControl: 'fixed-quality', quality: 24,
  });
  const view = new DataView(bytes.buffer);
  assert.equal(view.getUint32(16, true), 40_000_000);
  assert.equal(view.getUint32(28, true), 1);
  assert.equal(view.getUint32(32, true), 24);
});

test('parses bootstrap frames split across arbitrary WebSocket messages', () => {
  const encoded = encodeSignal(SIGNAL_TYPE.description, 'sdp-value', 'answer');
  const buffer = new SignalFrameBuffer();
  assert.deepEqual(buffer.push(encoded.subarray(0, 7)), []);
  assert.deepEqual(buffer.push(encoded.subarray(7, 22)), []);
  assert.deepEqual(buffer.push(encoded.subarray(22)), [{
    type: SIGNAL_TYPE.description,
    value: 'sdp-value',
    metadata: 'answer',
  }]);
});

test('decodes a valid stream header', () => {
  const bytes = new Uint8Array(44);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.stream, true);
  view.setUint16(4, 11, true);
  view.setUint16(6, 44, true);
  view.setUint32(8, MAGIC.av1, true);
  view.setUint32(12, 1920, true);
  view.setUint32(16, 1080, true);
  view.setUint32(20, 60, true);
  view.setUint32(24, 1, true);
  view.setUint32(28, 20_000_000, true);
  assert.equal(decodeStreamHeader(bytes).width, 1920);
});

test('decodes Host working and pacing bitrates', () => {
  const bytes = new Uint8Array(20);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.streamStatus, true);
  view.setUint16(4, 11, true);
  view.setUint16(6, 20, true);
  view.setUint32(8, 17_000_000, true);
  view.setBigUint64(12, 34_000_000n, true);
  assert.deepEqual(decodeStreamStatus(bytes), {
    magic: MAGIC.streamStatus,
    version: 11,
    headerSize: 20,
    mediaBitrateBps: 17_000_000,
    pacingBitrateBps: 34_000_000,
  });
});

test('decodes Host system cursor feedback', () => {
  const bytes = new Uint8Array(24);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.cursorState, true);
  view.setUint16(4, 11, true);
  view.setUint16(6, 24, true);
  view.setUint32(8, 7, true);
  view.setUint32(12, 49151, true);
  view.setUint32(16, 16384, true);
  view.setUint32(20, 9, true);
  assert.deepEqual(decodeCursorState(bytes), {
    magic: MAGIC.cursorState,
    version: 11,
    headerSize: 24,
    visible: true,
    embeddedInVideo: true,
    insideOutput: true,
    x: 49151,
    y: 16384,
    sequence: 9,
  });
});

test('round-trips Web clock synchronization fields', () => {
  const request = encodeClockSyncRequest(7, 123_456);
  assert.equal(new DataView(request.buffer).getBigUint64(16, true), 123_456n);
  const response = new Uint8Array(40);
  response.set(request.subarray(0, 16));
  const view = new DataView(response.buffer);
  view.setUint16(6, 40, true);
  view.setBigUint64(16, 123_456n, true);
  view.setBigUint64(24, 223_450n, true);
  view.setBigUint64(32, 223_455n, true);
  assert.equal(decodeClockSyncResponse(response).hostSendUs, 223_455);
});

test('decodes fixed-quality AV1 stream parameters', () => {
  const bytes = new Uint8Array(44);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.stream, true);
  view.setUint16(4, 11, true);
  view.setUint16(6, 44, true);
  view.setUint32(8, MAGIC.av1, true);
  view.setUint32(12, 2560, true);
  view.setUint32(16, 1440, true);
  view.setUint32(20, 60, true);
  view.setUint32(24, 1, true);
  view.setUint32(28, 20_000_000, true);
  view.setUint32(36, 1, true);
  view.setUint32(40, 28, true);
  const header = decodeStreamHeader(bytes);
  assert.equal(header.bitrateBps, 20_000_000);
  assert.equal(header.rateControl, 1);
  assert.equal(header.quality, 28);
});

test('decodes a valid H.264 stream header and profile', () => {
  const bytes = new Uint8Array(44);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.stream, true);
  view.setUint16(4, 11, true);
  view.setUint16(6, 44, true);
  view.setUint32(8, MAGIC.h264, true);
  view.setUint32(12, 1280, true);
  view.setUint32(16, 720, true);
  view.setUint32(20, 30, true);
  view.setUint32(24, 1, true);
  view.setUint32(28, 8_000_000, true);
  view.setUint32(32, 0x42e02a, true);
  const header = decodeStreamHeader(bytes);
  assert.equal(header.codec, MAGIC.h264);
  assert.equal(header.codecProfile, 0x42e02a);
  assert.equal(h264CodecString(header), 'avc1.42E02A');
});

test('reassembles a low-latency frame arriving out of chunk order', () => {
  const assembler = new VideoFrameAssembler();
  const frame = new Uint8Array(20_000).map((_, index) => index & 0xff);
  const makeChunk = (offset, length) => {
    const bytes = new Uint8Array(36 + length);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, MAGIC.videoChunk, true);
    view.setUint16(4, 11, true);
    view.setUint16(6, 36, true);
    view.setUint32(8, 1, true);
    view.setBigUint64(12, 42n, true);
    view.setBigUint64(20, 123456n, true);
    view.setUint32(28, frame.length, true);
    view.setUint32(32, offset, true);
    bytes.set(frame.subarray(offset, offset + length), 36);
    return bytes;
  };
  assert.equal(assembler.consume(makeChunk(16_384, frame.length - 16_384)).frame, null);
  const result = assembler.consume(makeChunk(0, 16_384)).frame;
  assert.equal(result.frameNumber, 42n);
  assert.deepEqual(result.data, frame);
});

test('holds delta frames after loss until a key frame arrives', () => {
  const gate = new VideoDecodeGate();
  const frame = (frameNumber, key = false) => ({ frameNumber, flags: key ? 1 : 0 });
  assert.equal(gate.evaluate(frame(40n, true)).frame.frameNumber, 40n);
  assert.equal(gate.evaluate(frame(41n)).frame.frameNumber, 41n);
  const gap = gate.evaluate(frame(43n));
  assert.equal(gap.frame, null);
  assert.equal(gap.recoveryStarted, true);
  assert.deepEqual(gate.evaluate(frame(44n)), { frame: null, recoveryStarted: false });
  assert.equal(gate.evaluate(frame(45n, true)).frame.frameNumber, 45n);
});

test('accepts native and browser-shaped invite URLs without exposing the fragment', () => {
  const native = parseInvite('wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT');
  assert.equal(native.websocket,
    'wss://signal.example.com/signal?session=V1StGXR8_Z5jdHi6B-myT&role=client');
  assert.equal(native.stun, 'stun:signal.example.com:3478');
  const browser = parseInvite('', { href: 'https://signal.example.com/#V1StGXR8_Z5jdHi6B-myT' });
  assert.equal(browser.websocket,
    'wss://signal.example.com/signal?session=V1StGXR8_Z5jdHi6B-myT&role=client');
  const pastedBrowser = parseInvite(
    'https://signal.example.com/#V1StGXR8_Z5jdHi6B-myT');
  assert.equal(pastedBrowser.websocket,
    'wss://signal.example.com/signal?session=V1StGXR8_Z5jdHi6B-myT&role=client');
});

test('encodes keyboard input and maps extended Windows scan codes', () => {
  assert.deepEqual(windowsScanCode('KeyA'), { scanCode: 0x1e, extended: false });
  assert.deepEqual(windowsScanCode('ArrowLeft'), { scanCode: 0x4b, extended: true });
  assert.equal(windowsScanCode('Pause'), null);
  const bytes = encodeInputEvent({ type: 9, flags: 3, value1: 0x4b, sequence: 17 });
  const view = new DataView(bytes.buffer);
  assert.equal(view.getUint16(8, true), 9);
  assert.equal(view.getUint16(10, true), 3);
  assert.equal(view.getInt32(12, true), 0x4b);
  assert.equal(view.getUint32(20, true), 17);
  const relative = encodeInputEvent({ type: 11, value1: -37, value2: 19 });
  const relativeView = new DataView(relative.buffer);
  assert.equal(relativeView.getUint16(8, true), 11);
  assert.equal(relativeView.getInt32(12, true), -37);
  assert.equal(relativeView.getInt32(16, true), 19);
  assert.throws(() => encodeInputEvent({ type: 12 }), /无效/);
});

test('round-trips UTF-8 clipboard text with a bounded variable-length frame', () => {
  const bytes = encodeClipboardText('remoe 剪贴板 🚀', 23);
  const view = new DataView(bytes.buffer);
  assert.equal(view.getUint32(0, true), MAGIC.clipboard);
  assert.equal(view.getUint16(6, true), 16);
  assert.equal(view.getUint32(8, true), bytes.length - 16);
  assert.deepEqual(decodeClipboardText(bytes), { text: 'remoe 剪贴板 🚀', sequence: 23 });
  assert.throws(() => decodeClipboardText(bytes.subarray(0, bytes.length - 1)), /无效/);
  assert.throws(() => encodeClipboardText('x'.repeat(1024 * 1024 + 1)), /1 MiB/);
});

test('maps touch gestures and virtual text to the existing input protocol', () => {
  const originalDocument = globalThis.document;
  const originalWindow = globalThis.window;
  const fakeDocument = new EventTarget();
  fakeDocument.pointerLockElement = null;
  fakeDocument.hidden = false;
  fakeDocument.exitPointerLock = () => {};
  const fakeWindow = new EventTarget();
  const target = new EventTarget();
  target.getBoundingClientRect = () => ({ left: 0, top: 0, width: 100, height: 50 });
  target.setPointerCapture = () => {};
  target.requestPointerLock = async () => {};
  globalThis.document = fakeDocument;
  globalThis.window = fakeWindow;

  const inputs = [];
  const viewportGestures = [];
  let viewportZoomed = false;
  const controller = new RemoteInputController(target, (event) => {
    inputs.push(event);
    return true;
  }, undefined, undefined, (gesture) => {
    if (gesture.type === 'pinch') viewportZoomed = true;
    if (gesture.type === 'pinch' || viewportZoomed) {
      viewportGestures.push(gesture);
      return true;
    }
    return false;
  });
  const pointer = (type, id, x, y) => {
    const event = new Event(type, { cancelable: true });
    Object.defineProperties(event, {
      pointerId: { value: id }, pointerType: { value: 'touch' },
      clientX: { value: x }, clientY: { value: y },
    });
    target.dispatchEvent(event);
  };

  try {
    controller.setTouchMode('trackpad');
    pointer('pointerdown', 1, 10, 10);
    pointer('pointermove', 1, 35, 10);
    pointer('pointerup', 1, 35, 10);
    assert.ok(inputs.some((event) => event.type === 1 && event.value1 > 32768));

    inputs.length = 0;
    pointer('pointerdown', 2, 20, 20);
    pointer('pointerup', 2, 20, 20);
    assert.deepEqual(inputs.slice(-2).map(({ type, flags = 0 }) => ({ type, flags })), [
      { type: 2, flags: 0 }, { type: 2, flags: 1 },
    ]);

    inputs.length = 0;
    assert.deepEqual(controller.sendText('A?中'), ['中']);
    assert.ok(inputs.some((event) => event.type === 9 && event.value1 === 0x2a));
    assert.ok(inputs.some((event) => event.type === 9 && event.value1 === 0x1e));
    assert.ok(inputs.some((event) => event.type === 9 && event.value1 === 0x35));

    controller.setTouchMode('trackpad');
    inputs.length = 0;
    pointer('pointerdown', 10, 30, 25);
    pointer('pointerdown', 11, 70, 25);
    pointer('pointermove', 11, 85, 25);
    pointer('pointerup', 11, 85, 25);
    pointer('pointerup', 10, 30, 25);
    assert.equal(inputs.length, 0);
    assert.ok(viewportGestures.some((gesture) =>
      gesture.type === 'pinch' && gesture.scale > 1));

    inputs.length = 0;
    viewportGestures.length = 0;
    pointer('pointerdown', 12, 20, 20);
    pointer('pointerdown', 13, 40, 20);
    pointer('pointermove', 12, 20, 30);
    pointer('pointermove', 13, 40, 30);
    pointer('pointerup', 13, 40, 30);
    pointer('pointerup', 12, 20, 30);
    assert.equal(inputs.length, 0);
    assert.ok(viewportGestures.some((gesture) => gesture.type === 'pan'));

    inputs.length = 0;
    viewportZoomed = false;
    pointer('pointerdown', 14, 20, 20);
    pointer('pointerdown', 15, 40, 20);
    pointer('pointermove', 14, 20, 30);
    pointer('pointermove', 15, 40, 30);
    pointer('pointerup', 15, 40, 30);
    pointer('pointerup', 14, 20, 30);
    assert.ok(inputs.some((event) => event.type === 7 && event.value1 > 0));

    inputs.length = 0;
    controller.setTouchMode('direct');
    pointer('pointerdown', 3, 50, 25);
    pointer('pointermove', 3, 75, 25);
    pointer('pointerup', 3, 75, 25);
    assert.deepEqual(inputs[0], { type: 1, value1: 32768, value2: 32768 });
    assert.deepEqual(inputs[1], { type: 2, flags: 0 });
    assert.ok(inputs.some((event) => event.type === 1 && event.value1 === 49151));
    assert.deepEqual(inputs.at(-1), { type: 2, flags: 1 });
  } finally {
    controller.dispose();
    globalThis.document = originalDocument;
    globalThis.window = originalWindow;
  }
});

test('maps unlocked desktop mouse input to absolute remote coordinates', () => {
  const originalDocument = globalThis.document;
  const originalWindow = globalThis.window;
  const fakeDocument = new EventTarget();
  fakeDocument.pointerLockElement = null;
  fakeDocument.hidden = false;
  fakeDocument.exitPointerLock = () => {};
  const fakeWindow = new EventTarget();
  const target = new EventTarget();
  target.getBoundingClientRect = () => ({ left: 10, top: 20, width: 200, height: 100 });
  target.requestPointerLock = async () => {};
  target.focus = () => target.dispatchEvent(new Event('focus'));
  globalThis.document = fakeDocument;
  globalThis.window = fakeWindow;

  const inputs = [];
  const controller = new RemoteInputController(target, (event) => {
    inputs.push(event);
    return true;
  });
  const mouse = (eventTarget, type, properties) => {
    const event = new Event(type, { cancelable: true });
    for (const [name, value] of Object.entries(properties)) {
      Object.defineProperty(event, name, { value });
    }
    eventTarget.dispatchEvent(event);
    return event;
  };

  try {
    mouse(target, 'mousemove', { clientX: 110, clientY: 70 });
    assert.deepEqual(inputs.at(-1), { type: 1, value1: 32768, value2: 32768 });

    const down = mouse(target, 'mousedown', { button: 2, clientX: 210, clientY: 120 });
    assert.equal(down.defaultPrevented, true);
    assert.deepEqual(inputs.slice(-2), [
      { type: 1, value1: 65535, value2: 65535 },
      { type: 3, flags: 0 },
    ]);

    mouse(fakeDocument, 'mouseup', { button: 2 });
    assert.deepEqual(inputs.at(-1), { type: 3, flags: 1 });

    const wheel = mouse(target, 'wheel', {
      clientX: 110, clientY: 70, deltaX: 0, deltaY: 40,
    });
    assert.equal(wheel.defaultPrevented, true);
    assert.deepEqual(inputs.slice(-2), [
      { type: 1, value1: 32768, value2: 32768 },
      { type: 7, value1: -40 },
    ]);

    const keyDown = mouse(fakeDocument, 'keydown', {
      code: 'KeyA', repeat: false, ctrlKey: false, altKey: false, shiftKey: false,
    });
    mouse(fakeDocument, 'keyup', {
      code: 'KeyA', repeat: false, ctrlKey: false, altKey: false, shiftKey: false,
    });
    assert.equal(keyDown.defaultPrevented, true);
    assert.deepEqual(inputs.slice(-2), [
      { type: 9, flags: 0, value1: 0x1e },
      { type: 9, flags: 1, value1: 0x1e },
    ]);
    assert.equal(controller.active, false);
  } finally {
    controller.dispose();
    globalThis.document = originalDocument;
    globalThis.window = originalWindow;
  }
});

test('automatically synchronizes clipboard text in both directions', async () => {
  class FakeClipboard extends EventTarget {
    onclipboardchange = null;
    text = 'local';
    writes = [];
    async readText() { return this.text; }
    async writeText(text) {
      this.text = text;
      this.writes.push(text);
    }
  }
  const clipboard = new FakeClipboard();
  const fakeDocument = new EventTarget();
  fakeDocument.hasFocus = () => true;
  const fakeWindow = new EventTarget();
  const sent = [];
  const synchronizer = new ClipboardSynchronizer(
    (text) => { sent.push(text); return true; },
    { clipboard, document: fakeDocument, window: fakeWindow },
  );

  synchronizer.start();
  await synchronizer.syncLocal();
  await Promise.resolve();
  assert.deepEqual(sent, ['local']);

  assert.equal(await synchronizer.receiveRemote('remote'), true);
  assert.deepEqual(clipboard.writes, ['remote']);

  clipboard.text = 'next local';
  clipboard.dispatchEvent(new Event('clipboardchange'));
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(sent, ['local', 'next local']);
  synchronizer.stop();
});

test('forwards Escape and releases desktop capture with Ctrl+Alt+Shift', async () => {
  const originalDocument = globalThis.document;
  const originalWindow = globalThis.window;
  const originalNavigator = globalThis.navigator;
  const fakeDocument = new EventTarget();
  const fakeWindow = new EventTarget();
  const fullscreenTarget = new EventTarget();
  const target = new EventTarget();
  const keyboardLocks = [];
  const pointerLockOptions = [];
  let keyboardUnlocks = 0;

  fakeDocument.pointerLockElement = null;
  fakeDocument.fullscreenElement = null;
  fakeDocument.hidden = false;
  fakeDocument.exitPointerLock = () => {
    fakeDocument.pointerLockElement = null;
    fakeDocument.dispatchEvent(new Event('pointerlockchange'));
  };
  fakeDocument.exitFullscreen = async () => { fakeDocument.fullscreenElement = null; };
  fullscreenTarget.requestFullscreen = async () => {
    fakeDocument.fullscreenElement = fullscreenTarget;
  };
  target.getBoundingClientRect = () => ({ left: 0, top: 0, width: 100, height: 50 });
  target.requestPointerLock = async (options) => {
    pointerLockOptions.push(options);
    fakeDocument.pointerLockElement = target;
    fakeDocument.dispatchEvent(new Event('pointerlockchange'));
  };

  globalThis.document = fakeDocument;
  globalThis.window = fakeWindow;
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      keyboard: {
        lock: async (keys) => { keyboardLocks.push(keys); },
        unlock: () => { keyboardUnlocks += 1; },
      },
    },
  });

  const inputs = [];
  const activeChanges = [];
  const controller = new RemoteInputController(
    target,
    (event) => { inputs.push(event); return true; },
    (active) => activeChanges.push(active),
  );
  const key = (type, code, modifiers = {}) => {
    const event = new Event(type, { cancelable: true });
    Object.defineProperties(event, {
      code: { value: code },
      repeat: { value: false },
      ctrlKey: { value: modifiers.ctrlKey ?? false },
      altKey: { value: modifiers.altKey ?? false },
      shiftKey: { value: modifiers.shiftKey ?? false },
    });
    fakeDocument.dispatchEvent(event);
    return event;
  };

  try {
    await controller.capture(fullscreenTarget);
    assert.equal(controller.active, true);
    assert.deepEqual(pointerLockOptions, [{ unadjustedMovement: true }]);
    assert.deepEqual(keyboardLocks, [['Escape']]);
    assert.equal(fakeDocument.fullscreenElement, fullscreenTarget);
    assert.equal(inputs.length, 0);

    const movement = new Event('mousemove');
    Object.defineProperties(movement, {
      movementX: { value: 17 },
      movementY: { value: -9 },
    });
    fakeDocument.dispatchEvent(movement);
    assert.deepEqual(inputs.at(-1), { type: 11, value1: 17, value2: -9 });

    key('keydown', 'Escape');
    key('keyup', 'Escape');
    assert.deepEqual(inputs.slice(-2), [
      { type: 9, flags: 0, value1: 0x01 },
      { type: 9, flags: 1, value1: 0x01 },
    ]);
    assert.equal(controller.active, true);

    key('keydown', 'ControlLeft', { ctrlKey: true });
    key('keydown', 'AltLeft', { ctrlKey: true, altKey: true });
    const release = key('keydown', 'ShiftLeft', {
      ctrlKey: true,
      altKey: true,
      shiftKey: true,
    });
    assert.equal(release.defaultPrevented, true);
    assert.equal(controller.active, false);
    assert.equal(fakeDocument.pointerLockElement, null);
    assert.ok(keyboardUnlocks >= 1);
    assert.deepEqual(inputs.slice(-2), [
      { type: 9, flags: 1, value1: 0x1d },
      { type: 9, flags: 1, value1: 0x38 },
    ]);
    assert.deepEqual(activeChanges, [true, false]);
  } finally {
    controller.dispose();
    globalThis.document = originalDocument;
    globalThis.window = originalWindow;
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: originalNavigator,
    });
  }
});

test('fits the whole remote frame inside differently shaped browser viewports', () => {
  assert.deepEqual(fitVideoSize(2560, 1440, 1920, 1000), { width: 1777, height: 1000 });
  assert.deepEqual(fitVideoSize(1280, 1024, 1920, 1080), { width: 1350, height: 1080 });
  const cursor = cursorViewportPosition(32768, 32768, {
    left: 100, top: 20, width: 1000, height: 500,
  });
  assert.ok(Math.abs(cursor.left - 600.00763) < 0.00001);
  assert.ok(Math.abs(cursor.top - 270.00381) < 0.00001);
});
