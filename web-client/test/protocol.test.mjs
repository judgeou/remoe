import { test } from 'vitest';
import assert from 'node:assert/strict';
import {
  MAGIC,
  SIGNAL_TYPE,
  SignalFrameBuffer,
  VideoFrameAssembler,
  decodeStreamHeader,
  encodeClientConfig,
  encodeInputEvent,
  encodeSignal,
} from '../src/core/protocol.js';
import { parseInvite } from '../src/core/remoe-client.js';
import { RemoteInputController, windowsScanCode } from '../src/core/input.js';
import { cursorViewportPosition, fitVideoSize } from '../src/core/layout.js';

test('encodes protocol v7 client settings as little-endian packed bytes', () => {
  const bytes = encodeClientConfig({ fps: 90, bitrateMbps: 25, scalePercent: 75 });
  const view = new DataView(bytes.buffer);
  assert.equal(bytes.length, 28);
  assert.equal(view.getUint32(0, true), MAGIC.clientConfig);
  assert.equal(view.getUint16(4, true), 7);
  assert.equal(view.getUint32(8, true), 90);
  assert.equal(view.getUint32(16, true), 25_000_000);
  assert.equal(view.getUint32(20, true), 75);
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
  const bytes = new Uint8Array(36);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.stream, true);
  view.setUint16(4, 7, true);
  view.setUint16(6, 36, true);
  view.setUint32(8, MAGIC.av1, true);
  view.setUint32(12, 1920, true);
  view.setUint32(16, 1080, true);
  view.setUint32(20, 60, true);
  view.setUint32(24, 1, true);
  view.setUint32(28, 20_000_000, true);
  assert.equal(decodeStreamHeader(bytes).width, 1920);
});

test('reassembles an AV1 frame arriving out of chunk order', () => {
  const assembler = new VideoFrameAssembler();
  const frame = new Uint8Array(20_000).map((_, index) => index & 0xff);
  const makeChunk = (offset, length) => {
    const bytes = new Uint8Array(36 + length);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, MAGIC.videoChunk, true);
    view.setUint16(4, 7, true);
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
});

test('maps touch gestures and virtual text to the existing input protocol', () => {
  const originalDocument = globalThis.document;
  const originalWindow = globalThis.window;
  const fakeDocument = new EventTarget();
  fakeDocument.pointerLockElement = null;
  fakeDocument.hidden = false;
  fakeDocument.exitPointerLock = () => {};
  const fakeWindow = new EventTarget();
  const canvas = new EventTarget();
  canvas.getBoundingClientRect = () => ({ left: 0, top: 0, width: 100, height: 50 });
  canvas.setPointerCapture = () => {};
  canvas.requestPointerLock = async () => {};
  globalThis.document = fakeDocument;
  globalThis.window = fakeWindow;

  const inputs = [];
  const controller = new RemoteInputController(canvas, (event) => {
    inputs.push(event);
    return true;
  });
  const pointer = (type, id, x, y) => {
    const event = new Event(type, { cancelable: true });
    Object.defineProperties(event, {
      pointerId: { value: id }, pointerType: { value: 'touch' },
      clientX: { value: x }, clientY: { value: y },
    });
    canvas.dispatchEvent(event);
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

test('fits the whole remote frame inside differently shaped browser viewports', () => {
  assert.deepEqual(fitVideoSize(2560, 1440, 1920, 1000), { width: 1777, height: 1000 });
  assert.deepEqual(fitVideoSize(1280, 1024, 1920, 1080), { width: 1350, height: 1080 });
  const cursor = cursorViewportPosition(32768, 32768, {
    left: 100, top: 20, width: 1000, height: 500,
  });
  assert.ok(Math.abs(cursor.left - 600.00763) < 0.00001);
  assert.ok(Math.abs(cursor.top - 270.00381) < 0.00001);
});
