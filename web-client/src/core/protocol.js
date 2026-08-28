export const PROTOCOL_VERSION = 10;

export const MAGIC = Object.freeze({
  clientConfig: 0x46434d52,
  stream: 0x454f4d52,
  streamReady: 0x59445253,
  input: 0x54504e49,
  clipboard: 0x50494c43,
  signal: 0x534d5257,
  av1: 0x31305641,
  h264: 0x34363248,
});

export const SIGNAL_TYPE = Object.freeze({
  description: 1,
  candidate: 2,
  ready: 3,
  acknowledged: 4,
});

export const RATE_CONTROL = Object.freeze({ cbr: 0, fixedQuality: 1 });
export const INPUT_FLAG_RELEASE = 1;
export const INPUT_FLAG_EXTENDED_KEY = 2;

export const INPUT_TYPE = Object.freeze({
  mouseMove: 1,
  mouseLeft: 2,
  mouseRight: 3,
  mouseMiddle: 4,
  mouseX1: 5,
  mouseX2: 6,
  mouseWheel: 7,
  mouseHorizontalWheel: 8,
  keyboard: 9,
});

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
export const MAX_CLIPBOARD_TEXT_SIZE = 1024 * 1024;

export function encodeSignal(type, value = '', metadata = '') {
  const valueBytes = encoder.encode(value);
  const metadataBytes = encoder.encode(metadata);
  const bytes = new Uint8Array(20 + valueBytes.length + metadataBytes.length);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.signal, true);
  view.setUint16(4, PROTOCOL_VERSION, true);
  view.setUint16(6, 20, true);
  view.setUint16(8, type, true);
  view.setUint16(10, 0, true);
  view.setUint32(12, valueBytes.length, true);
  view.setUint32(16, metadataBytes.length, true);
  bytes.set(valueBytes, 20);
  bytes.set(metadataBytes, 20 + valueBytes.length);
  return bytes;
}

export class SignalFrameBuffer {
  #bytes = new Uint8Array();

  push(value) {
    const incoming = value instanceof Uint8Array ? value : new Uint8Array(value);
    const combined = new Uint8Array(this.#bytes.length + incoming.length);
    combined.set(this.#bytes);
    combined.set(incoming, this.#bytes.length);
    this.#bytes = combined;

    const frames = [];
    while (this.#bytes.length >= 20) {
      const view = new DataView(this.#bytes.buffer, this.#bytes.byteOffset);
      if (view.getUint32(0, true) !== MAGIC.signal ||
          view.getUint16(4, true) !== PROTOCOL_VERSION ||
          view.getUint16(6, true) !== 20 || view.getUint16(10, true) !== 0) {
        throw new Error('信令服务器转发了无效的 protocol v10 bootstrap 帧');
      }
      const type = view.getUint16(8, true);
      const valueSize = view.getUint32(12, true);
      const metadataSize = view.getUint32(16, true);
      if (![1, 2, 3, 4].includes(type) || valueSize > 1024 * 1024 || metadataSize > 256) {
        throw new Error('信令 bootstrap 帧长度或类型无效');
      }
      if ((type === SIGNAL_TYPE.ready || type === SIGNAL_TYPE.acknowledged) &&
          (valueSize !== 0 || metadataSize !== 0)) {
        throw new Error('信令 bootstrap marker 携带了意外数据');
      }
      const frameSize = 20 + valueSize + metadataSize;
      if (this.#bytes.length < frameSize) break;
      frames.push({
        type,
        value: decoder.decode(this.#bytes.subarray(20, 20 + valueSize)),
        metadata: decoder.decode(this.#bytes.subarray(20 + valueSize, frameSize)),
      });
      this.#bytes = this.#bytes.slice(frameSize);
    }
    return frames;
  }
}

export function encodeClientConfig({
  fps = 60,
  bitrateMbps = 20,
  scalePercent = 100,
  rateControl = 'cbr',
  quality = 28,
} = {}) {
  const fixedQuality = rateControl === 'fixed-quality';
  if (!Number.isInteger(fps) || fps < 1 || fps > 240 ||
      (!fixedQuality && (!Number.isFinite(bitrateMbps) || bitrateMbps < 1 || bitrateMbps > 1000)) ||
      (fixedQuality && (!Number.isInteger(quality) || quality < 1 || quality > 51)) ||
      (!fixedQuality && rateControl !== 'cbr') ||
      !Number.isInteger(scalePercent) || scalePercent < 10 || scalePercent > 100) {
    throw new RangeError('无效的视频请求参数');
  }
  const bitrate = fixedQuality ? 0 : Math.round(bitrateMbps * 1_000_000);
  if (bitrate > 1_000_000_000) throw new RangeError('码率超过 protocol v10 上限');
  const bytes = new Uint8Array(36);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.clientConfig, true);
  view.setUint16(4, PROTOCOL_VERSION, true);
  view.setUint16(6, 36, true);
  view.setUint32(8, fps, true);
  view.setUint32(12, 1, true);
  view.setUint32(16, bitrate, true);
  view.setUint32(20, scalePercent, true);
  view.setUint32(24, 1, true); // Supports bidirectional UTF-8 clipboard text.
  view.setUint32(28, fixedQuality ? RATE_CONTROL.fixedQuality : RATE_CONTROL.cbr, true);
  view.setUint32(32, fixedQuality ? quality : 0, true);
  return bytes;
}

export function decodeStreamHeader(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(value);
  if (bytes.byteLength !== 44) throw new Error('Host 返回的 StreamHeader 长度错误');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const result = {
    magic: view.getUint32(0, true),
    version: view.getUint16(4, true),
    headerSize: view.getUint16(6, true),
    codec: view.getUint32(8, true),
    width: view.getUint32(12, true),
    height: view.getUint32(16, true),
    fpsNum: view.getUint32(20, true),
    fpsDen: view.getUint32(24, true),
    bitrateBps: view.getUint32(28, true),
    codecProfile: view.getUint32(32, true),
    rateControl: view.getUint32(36, true),
    quality: view.getUint32(40, true),
  };
  const codecValid = result.codec === MAGIC.av1 || result.codec === MAGIC.h264;
  const profileValid = result.codec === MAGIC.av1
    ? result.codecProfile === 0
    : result.codecProfile > 0 && result.codecProfile <= 0xffffff;
  const rateControlValid = result.rateControl === RATE_CONTROL.cbr
    ? result.bitrateBps >= 1_000_000 && result.quality === 0
    : result.codec === MAGIC.av1 && result.rateControl === RATE_CONTROL.fixedQuality &&
      result.bitrateBps === 0 && result.quality >= 1 && result.quality <= 51;
  if (result.magic !== MAGIC.stream || result.version !== PROTOCOL_VERSION ||
      result.headerSize !== 44 || !codecValid || !profileValid || !rateControlValid ||
      result.width < 2 || result.height < 2 || result.fpsNum < 1 ||
      result.fpsDen !== 1) {
    throw new Error('Host 返回了无效或不受支持的 StreamHeader');
  }
  return result;
}

export function encodeStreamReady() {
  const bytes = new Uint8Array(8);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.streamReady, true);
  view.setUint16(4, PROTOCOL_VERSION, true);
  view.setUint16(6, 8, true);
  return bytes;
}

export function encodeInputEvent({ type, flags = 0, value1 = 0, value2 = 0, sequence = 0 }) {
  if (!Number.isInteger(type) || type < INPUT_TYPE.mouseMove ||
      type > INPUT_TYPE.keyboard || !Number.isInteger(flags) || flags < 0 || flags > 3 ||
      !Number.isInteger(value1) || !Number.isInteger(value2)) {
    throw new RangeError('无效的键鼠输入事件');
  }
  const bytes = new Uint8Array(24);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.input, true);
  view.setUint16(4, PROTOCOL_VERSION, true);
  view.setUint16(6, 24, true);
  view.setUint16(8, type, true);
  view.setUint16(10, flags, true);
  view.setInt32(12, value1, true);
  view.setInt32(16, value2, true);
  view.setUint32(20, sequence >>> 0, true);
  return bytes;
}

export function encodeClipboardText(text, sequence = 0) {
  if (typeof text !== 'string') throw new TypeError('剪贴板内容必须是文本');
  const payload = encoder.encode(text);
  if (payload.byteLength > MAX_CLIPBOARD_TEXT_SIZE) {
    throw new RangeError('剪贴板文本超过 1 MiB 上限');
  }
  const bytes = new Uint8Array(16 + payload.byteLength);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC.clipboard, true);
  view.setUint16(4, PROTOCOL_VERSION, true);
  view.setUint16(6, 16, true);
  view.setUint32(8, payload.byteLength, true);
  view.setUint32(12, sequence >>> 0, true);
  bytes.set(payload, 16);
  return bytes;
}

export function decodeClipboardText(value) {
  const bytes = value instanceof Uint8Array ? value : new Uint8Array(value);
  if (bytes.byteLength < 16) throw new Error('Host 发来了截断的剪贴板消息');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const payloadSize = view.getUint32(8, true);
  if (view.getUint32(0, true) !== MAGIC.clipboard ||
      view.getUint16(4, true) !== PROTOCOL_VERSION || view.getUint16(6, true) !== 16 ||
      payloadSize > MAX_CLIPBOARD_TEXT_SIZE || bytes.byteLength !== 16 + payloadSize) {
    throw new Error('Host 发来了无效的剪贴板消息');
  }
  return {
    text: decoder.decode(bytes.subarray(16)),
    sequence: view.getUint32(12, true),
  };
}

export function av1CodecString({ width, height, fpsNum, fpsDen = 1 }) {
  const pixels = width * height;
  const samplesPerSecond = pixels * fpsNum / fpsDen;
  let level = 13; // Level 5.1, enough for 4K60.
  if (pixels <= 2_359_296 && samplesPerSecond <= 70_778_880) level = 8;
  else if (pixels <= 2_359_296 && samplesPerSecond <= 141_557_760) level = 9;
  else if (pixels <= 8_912_896 && samplesPerSecond <= 267_386_880) level = 12;
  return `av01.0.${String(level).padStart(2, '0')}M.08`;
}

export function h264CodecString({ codecProfile }) {
  if (!Number.isInteger(codecProfile) || codecProfile <= 0 || codecProfile > 0xffffff) {
    throw new RangeError('无效的 H.264 profile-level-id');
  }
  return `avc1.${codecProfile.toString(16).padStart(6, '0').toUpperCase()}`;
}
