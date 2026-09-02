import {
  INPUT_FLAG_EXTENDED_KEY,
  INPUT_FLAG_RELEASE,
  INPUT_TYPE,
} from './protocol.js';

const scanCodes = new Map(Object.entries({
  Escape: [0x01, false],
  Digit1: [0x02, false], Digit2: [0x03, false], Digit3: [0x04, false],
  Digit4: [0x05, false], Digit5: [0x06, false], Digit6: [0x07, false],
  Digit7: [0x08, false], Digit8: [0x09, false], Digit9: [0x0a, false],
  Digit0: [0x0b, false], Minus: [0x0c, false], Equal: [0x0d, false],
  Backspace: [0x0e, false], Tab: [0x0f, false],
  KeyQ: [0x10, false], KeyW: [0x11, false], KeyE: [0x12, false], KeyR: [0x13, false],
  KeyT: [0x14, false], KeyY: [0x15, false], KeyU: [0x16, false], KeyI: [0x17, false],
  KeyO: [0x18, false], KeyP: [0x19, false], BracketLeft: [0x1a, false],
  BracketRight: [0x1b, false], Enter: [0x1c, false], ControlLeft: [0x1d, false],
  KeyA: [0x1e, false], KeyS: [0x1f, false], KeyD: [0x20, false], KeyF: [0x21, false],
  KeyG: [0x22, false], KeyH: [0x23, false], KeyJ: [0x24, false], KeyK: [0x25, false],
  KeyL: [0x26, false], Semicolon: [0x27, false], Quote: [0x28, false],
  Backquote: [0x29, false], ShiftLeft: [0x2a, false], Backslash: [0x2b, false],
  KeyZ: [0x2c, false], KeyX: [0x2d, false], KeyC: [0x2e, false], KeyV: [0x2f, false],
  KeyB: [0x30, false], KeyN: [0x31, false], KeyM: [0x32, false], Comma: [0x33, false],
  Period: [0x34, false], Slash: [0x35, false], ShiftRight: [0x36, false],
  NumpadMultiply: [0x37, false], AltLeft: [0x38, false], Space: [0x39, false],
  CapsLock: [0x3a, false], F1: [0x3b, false], F2: [0x3c, false], F3: [0x3d, false],
  F4: [0x3e, false], F5: [0x3f, false], F6: [0x40, false], F7: [0x41, false],
  F8: [0x42, false], F9: [0x43, false], F10: [0x44, false], NumLock: [0x45, false],
  ScrollLock: [0x46, false], Numpad7: [0x47, false], Numpad8: [0x48, false],
  Numpad9: [0x49, false], NumpadSubtract: [0x4a, false], Numpad4: [0x4b, false],
  Numpad5: [0x4c, false], Numpad6: [0x4d, false], NumpadAdd: [0x4e, false],
  Numpad1: [0x4f, false], Numpad2: [0x50, false], Numpad3: [0x51, false],
  Numpad0: [0x52, false], NumpadDecimal: [0x53, false], IntlBackslash: [0x56, false],
  F11: [0x57, false], F12: [0x58, false],
  NumpadEnter: [0x1c, true], ControlRight: [0x1d, true], NumpadDivide: [0x35, true],
  PrintScreen: [0x37, true], AltRight: [0x38, true], Home: [0x47, true],
  ArrowUp: [0x48, true], PageUp: [0x49, true], ArrowLeft: [0x4b, true],
  ArrowRight: [0x4d, true], End: [0x4f, true], ArrowDown: [0x50, true],
  PageDown: [0x51, true], Insert: [0x52, true], Delete: [0x53, true],
  MetaLeft: [0x5b, true], MetaRight: [0x5c, true], ContextMenu: [0x5d, true],
}));

const mouseTypes = new Map([
  [0, INPUT_TYPE.mouseLeft],
  [1, INPUT_TYPE.mouseMiddle],
  [2, INPUT_TYPE.mouseRight],
  [3, INPUT_TYPE.mouseX1],
  [4, INPUT_TYPE.mouseX2],
]);

const twoFingerThreshold = 8;

const printableKeys = new Map();
for (const letter of 'abcdefghijklmnopqrstuvwxyz') {
  printableKeys.set(letter, [`Key${letter.toUpperCase()}`, false]);
  printableKeys.set(letter.toUpperCase(), [`Key${letter.toUpperCase()}`, true]);
}
for (const [plain, shifted, code] of [
  ['1', '!', 'Digit1'], ['2', '@', 'Digit2'], ['3', '#', 'Digit3'],
  ['4', '$', 'Digit4'], ['5', '%', 'Digit5'], ['6', '^', 'Digit6'],
  ['7', '&', 'Digit7'], ['8', '*', 'Digit8'], ['9', '(', 'Digit9'],
  ['0', ')', 'Digit0'], ['-', '_', 'Minus'], ['=', '+', 'Equal'],
  ['[', '{', 'BracketLeft'], [']', '}', 'BracketRight'], ['\\', '|', 'Backslash'],
  [';', ':', 'Semicolon'], ["'", '"', 'Quote'], ['`', '~', 'Backquote'],
  [',', '<', 'Comma'], ['.', '>', 'Period'], ['/', '?', 'Slash'],
]) {
  printableKeys.set(plain, [code, false]);
  printableKeys.set(shifted, [code, true]);
}
printableKeys.set(' ', ['Space', false]);

export function windowsScanCode(code) {
  const value = scanCodes.get(code);
  return value ? { scanCode: value[0], extended: value[1] } : null;
}

function wheelDelta(value) {
  if (!Number.isFinite(value) || value === 0) return 0;
  return -Math.sign(value) * Math.max(1, Math.min(120, Math.round(Math.abs(value))));
}

export class RemoteInputController {
  #target;
  #send;
  #onActiveChanged;
  #onPointerMoved;
  #onViewportGesture;
  #active = false;
  #pressedKeys = new Map();
  #pressedButtons = new Set();
  #x = 32768;
  #y = 32768;
  #listeners = [];
  #touchMode = null;
  #touches = new Map();
  #touchGesture = null;
  #touchDrag = false;
  #directPressed = false;
  #lastTap = null;

  /**
   * @param {HTMLElement} target
   * @param {(event: {type: number, flags?: number, value1?: number, value2?: number}) => boolean} send
   * @param {(active: boolean) => void} onActiveChanged
   * @param {(position: {x: number, y: number}) => void} onPointerMoved
   * @param {(gesture: ({type: 'pan', deltaX: number, deltaY: number} |
   *   {type: 'pinch', scale: number, clientX: number, clientY: number,
   *    deltaX: number, deltaY: number})) => boolean} onViewportGesture
   */
  constructor(target, send, onActiveChanged = () => {}, onPointerMoved = () => {},
              onViewportGesture = () => false) {
    this.#target = target;
    this.#send = send;
    this.#onActiveChanged = onActiveChanged;
    this.#onPointerMoved = onPointerMoved;
    this.#onViewportGesture = onViewportGesture;
    this.#listen(document, 'pointerlockchange', () => this.#pointerLockChanged());
    this.#listen(document, 'mousemove', (event) => this.#mouseMove(event));
    this.#listen(document, 'mousedown', (event) => this.#mouseButton(event, false));
    this.#listen(document, 'mouseup', (event) => this.#mouseButton(event, true));
    this.#listen(document, 'wheel', (event) => this.#wheel(event), { passive: false });
    this.#listen(document, 'keydown', (event) => this.#keyboard(event, false));
    this.#listen(document, 'keyup', (event) => this.#keyboard(event, true));
    this.#listen(this.#target, 'pointerdown', (event) => this.#touchPointerDown(event));
    this.#listen(this.#target, 'pointermove', (event) => this.#touchPointerMove(event));
    this.#listen(this.#target, 'pointerup', (event) => this.#touchPointerUp(event));
    this.#listen(this.#target, 'pointercancel', (event) => this.#touchPointerUp(event));
    this.#listen(window, 'blur', () => this.releaseAll());
    this.#listen(document, 'visibilitychange', () => {
      if (document.hidden) this.releaseAll();
    });
    this.#listen(this.#target, 'contextmenu', (event) => event.preventDefault());
  }

  get active() { return this.#active; }
  get touchMode() { return this.#touchMode; }

  async capture() {
    this.#touchMode = null;
    try {
      await this.#target.requestPointerLock({ unadjustedMovement: true });
    } catch {
      await this.#target.requestPointerLock();
    }
  }

  setTouchMode(mode) {
    if (mode !== 'trackpad' && mode !== 'direct') {
      throw new TypeError('触控模式必须是 trackpad 或 direct');
    }
    this.releaseAll();
    this.#touchMode = mode;
    this.#touches.clear();
    this.#touchGesture = null;
    if (document.pointerLockElement === this.#target) document.exitPointerLock();
    this.#setActive(true);
  }

  tapKey(code) {
    const key = windowsScanCode(code);
    if (!key) return false;
    this.#sendKey(code, key, false);
    this.#sendKey(code, key, true);
    return true;
  }

  setModifier(code, pressed) {
    const key = windowsScanCode(code);
    if (!key) return false;
    this.#sendKey(code, key, !pressed);
    return true;
  }

  sendText(text) {
    const unsupported = [];
    for (const character of text) {
      if (character === '\n' || character === '\r') {
        this.tapKey('Enter');
        continue;
      }
      const stroke = printableKeys.get(character);
      if (!stroke) {
        unsupported.push(character);
        continue;
      }
      const [code, needsShift] = stroke;
      const shiftHeld = this.#pressedKeys.has('ShiftLeft') || this.#pressedKeys.has('ShiftRight');
      if (needsShift && !shiftHeld) this.setModifier('ShiftLeft', true);
      this.tapKey(code);
      if (needsShift && !shiftHeld) this.setModifier('ShiftLeft', false);
    }
    return unsupported;
  }

  tapMouseButton(button = 'left') {
    const type = button === 'right' ? INPUT_TYPE.mouseRight : INPUT_TYPE.mouseLeft;
    this.#send({ type, flags: 0 });
    this.#send({ type, flags: INPUT_FLAG_RELEASE });
  }

  releaseAll() {
    for (const { scanCode, extended } of this.#pressedKeys.values()) {
      this.#send({
        type: INPUT_TYPE.keyboard,
        flags: INPUT_FLAG_RELEASE | (extended ? INPUT_FLAG_EXTENDED_KEY : 0),
        value1: scanCode,
      });
    }
    for (const type of this.#pressedButtons) {
      this.#send({ type, flags: INPUT_FLAG_RELEASE });
    }
    this.#pressedKeys.clear();
    this.#pressedButtons.clear();
    this.#touchDrag = false;
    this.#directPressed = false;
    this.#touches.clear();
    this.#touchGesture = null;
    this.#lastTap = null;
  }

  dispose() {
    this.releaseAll();
    if (document.pointerLockElement === this.#target) document.exitPointerLock();
    for (const [target, type, listener, options] of this.#listeners) {
      target.removeEventListener(type, listener, options);
    }
    this.#listeners = [];
    this.#touchMode = null;
    this.#touches.clear();
    this.#setActive(false);
  }

  #listen(target, type, listener, options) {
    target.addEventListener(type, listener, options);
    this.#listeners.push([target, type, listener, options]);
  }

  #pointerLockChanged() {
    const pointerLocked = document.pointerLockElement === this.#target;
    const active = pointerLocked || this.#touchMode !== null;
    if (!active) this.releaseAll();
    else {
      if (pointerLocked) {
        this.#x = 32768;
        this.#y = 32768;
        this.#emitPointer();
      }
    }
    this.#setActive(active);
  }

  #setActive(active) {
    if (this.#active === active) return;
    this.#active = active;
    this.#onActiveChanged(active);
  }

  #mouseMove(event) {
    if (document.pointerLockElement !== this.#target) return;
    const rect = this.#target.getBoundingClientRect();
    if (rect.width < 2 || rect.height < 2) return;
    this.#x = Math.max(0, Math.min(65535, this.#x + event.movementX * 65535 / rect.width));
    this.#y = Math.max(0, Math.min(65535, this.#y + event.movementY * 65535 / rect.height));
    this.#emitPointer();
  }

  #emitPointer() {
    const x = Math.round(this.#x);
    const y = Math.round(this.#y);
    this.#send({
      type: INPUT_TYPE.mouseMove,
      value1: x,
      value2: y,
    });
    this.#onPointerMoved({ x, y });
  }

  #moveRelative(deltaX, deltaY) {
    const rect = this.#target.getBoundingClientRect();
    if (rect.width < 2 || rect.height < 2) return;
    this.#x = Math.max(0, Math.min(65535, this.#x + deltaX * 65535 / rect.width));
    this.#y = Math.max(0, Math.min(65535, this.#y + deltaY * 65535 / rect.height));
    this.#emitPointer();
  }

  #moveAbsolute(clientX, clientY) {
    const rect = this.#target.getBoundingClientRect();
    if (rect.width < 2 || rect.height < 2) return;
    this.#x = Math.max(0, Math.min(65535, (clientX - rect.left) * 65535 / rect.width));
    this.#y = Math.max(0, Math.min(65535, (clientY - rect.top) * 65535 / rect.height));
    this.#emitPointer();
  }

  #touchPointerDown(event) {
    if (!this.#touchMode || (event.pointerType !== 'touch' && event.pointerType !== 'pen')) return;
    event.preventDefault();
    this.#target.setPointerCapture?.(event.pointerId);
    const point = { x: event.clientX, y: event.clientY };
    this.#touches.set(event.pointerId, point);
    if (!this.#touchGesture) {
      this.#touchGesture = {
        startedAt: performance.now(),
        start: point,
        lastCenter: point,
        maxPointers: 1,
        distance: 0,
      };
    }

    if (this.#touchMode === 'direct') {
      this.#moveAbsolute(event.clientX, event.clientY);
      if (!this.#directPressed) {
        this.#directPressed = true;
        this.#pressedButtons.add(INPUT_TYPE.mouseLeft);
        this.#send({ type: INPUT_TYPE.mouseLeft, flags: 0 });
      }
      return;
    }

    if (this.#touches.size >= 2) {
      this.#touchGesture.maxPointers = 2;
      const center = this.#touchCenter();
      const spread = this.#touchSpread();
      Object.assign(this.#touchGesture, {
        lastCenter: center,
        twoFingerStartCenter: center,
        lastSpread: spread,
        startSpread: spread,
        twoFingerMode: null,
      });
      if (this.#touchDrag) {
        this.#send({ type: INPUT_TYPE.mouseLeft, flags: INPUT_FLAG_RELEASE });
        this.#pressedButtons.delete(INPUT_TYPE.mouseLeft);
        this.#touchDrag = false;
      }
      return;
    }

    const now = performance.now();
    if (this.#lastTap && now - this.#lastTap.time < 350 &&
        Math.hypot(point.x - this.#lastTap.x, point.y - this.#lastTap.y) < 28) {
      this.#touchDrag = true;
      this.#lastTap = null;
      this.#pressedButtons.add(INPUT_TYPE.mouseLeft);
      this.#send({ type: INPUT_TYPE.mouseLeft, flags: 0 });
    }
  }

  #touchPointerMove(event) {
    const previous = this.#touches.get(event.pointerId);
    if (!this.#touchMode || !previous) return;
    event.preventDefault();
    const point = { x: event.clientX, y: event.clientY };
    this.#touches.set(event.pointerId, point);
    const deltaX = point.x - previous.x;
    const deltaY = point.y - previous.y;
    if (this.#touchGesture) this.#touchGesture.distance += Math.hypot(deltaX, deltaY);

    if (this.#touchMode === 'direct') {
      this.#moveAbsolute(point.x, point.y);
    } else if (this.#touches.size === 1 && this.#touchGesture?.maxPointers === 1) {
      this.#moveRelative(deltaX, deltaY);
    } else if (this.#touches.size >= 2 && this.#touchGesture) {
      const center = this.#touchCenter();
      const spread = this.#touchSpread();
      const gesture = this.#touchGesture;
      if (!gesture.twoFingerMode) {
        const pan = Math.hypot(
          center.x - gesture.twoFingerStartCenter.x,
          center.y - gesture.twoFingerStartCenter.y,
        );
        const pinch = Math.abs(spread - gesture.startSpread);
        if (Math.max(pan, pinch) >= twoFingerThreshold) {
          gesture.twoFingerMode = pinch > pan ? 'pinch' : 'scroll';
        }
      }

      const deltaX = center.x - gesture.lastCenter.x;
      const deltaY = center.y - gesture.lastCenter.y;
      if (gesture.twoFingerMode === 'scroll') {
        const consumed = this.#onViewportGesture({ type: 'pan', deltaX, deltaY });
        if (!consumed) {
          const scrollX = Math.round(deltaX * 3);
          const scrollY = Math.round(deltaY * 3);
          if (scrollY) this.#send({ type: INPUT_TYPE.mouseWheel, value1: scrollY });
          if (scrollX) this.#send({ type: INPUT_TYPE.mouseHorizontalWheel, value1: scrollX });
        }
      } else if (gesture.twoFingerMode === 'pinch' && gesture.lastSpread > 0 && spread > 0) {
        this.#onViewportGesture({
          type: 'pinch',
          scale: spread / gesture.lastSpread,
          clientX: center.x,
          clientY: center.y,
          deltaX,
          deltaY,
        });
      }
      this.#touchGesture.lastCenter = center;
      this.#touchGesture.lastSpread = spread;
    }
  }

  #touchPointerUp(event) {
    if (!this.#touchMode || !this.#touches.has(event.pointerId)) return;
    event.preventDefault();
    this.#touches.delete(event.pointerId);

    if (this.#touchMode === 'direct') {
      if (this.#directPressed && this.#touches.size === 0) {
        this.#send({ type: INPUT_TYPE.mouseLeft, flags: INPUT_FLAG_RELEASE });
        this.#pressedButtons.delete(INPUT_TYPE.mouseLeft);
        this.#directPressed = false;
        this.#touchGesture = null;
      }
      return;
    }

    if (this.#touches.size > 0) {
      if (this.#touchGesture) this.#touchGesture.lastCenter = this.#touchCenter();
      return;
    }

    if (this.#touchDrag) {
      this.#send({ type: INPUT_TYPE.mouseLeft, flags: INPUT_FLAG_RELEASE });
      this.#pressedButtons.delete(INPUT_TYPE.mouseLeft);
      this.#touchDrag = false;
    } else if (this.#touchGesture) {
      const elapsed = performance.now() - this.#touchGesture.startedAt;
      if (!this.#touchGesture.twoFingerMode && elapsed < 500 &&
          this.#touchGesture.distance < 18) {
        if (this.#touchGesture.maxPointers === 2) {
          this.tapMouseButton('right');
          this.#lastTap = null;
        } else {
          this.tapMouseButton('left');
          this.#lastTap = {
            time: performance.now(),
            x: this.#touchGesture.start.x,
            y: this.#touchGesture.start.y,
          };
        }
      }
    }
    this.#touchGesture = null;
  }

  #touchCenter() {
    let x = 0;
    let y = 0;
    for (const point of this.#touches.values()) {
      x += point.x;
      y += point.y;
    }
    return { x: x / this.#touches.size, y: y / this.#touches.size };
  }

  #touchSpread() {
    const points = [...this.#touches.values()];
    if (points.length < 2) return 0;
    return Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);
  }

  #mouseButton(event, release) {
    if (document.pointerLockElement !== this.#target) return;
    const type = mouseTypes.get(event.button);
    if (!type) return;
    event.preventDefault();
    if (release) this.#pressedButtons.delete(type);
    else this.#pressedButtons.add(type);
    this.#send({ type, flags: release ? INPUT_FLAG_RELEASE : 0 });
  }

  #wheel(event) {
    if (document.pointerLockElement !== this.#target) return;
    event.preventDefault();
    const vertical = wheelDelta(event.deltaY);
    const horizontal = wheelDelta(event.deltaX);
    if (vertical) this.#send({ type: INPUT_TYPE.mouseWheel, value1: vertical });
    if (horizontal) this.#send({ type: INPUT_TYPE.mouseHorizontalWheel, value1: horizontal });
  }

  #keyboard(event, release) {
    if (!this.#active || event.repeat) return;
    const key = windowsScanCode(event.code);
    if (!key) return;
    event.preventDefault();
    this.#sendKey(event.code, key, release);
  }

  #sendKey(code, key, release) {
    if (release && !this.#pressedKeys.has(code)) return;
    if (!release && this.#pressedKeys.has(code)) return;
    if (release) this.#pressedKeys.delete(code);
    else this.#pressedKeys.set(code, key);
    this.#send({
      type: INPUT_TYPE.keyboard,
      flags: (release ? INPUT_FLAG_RELEASE : 0) |
        (key.extended ? INPUT_FLAG_EXTENDED_KEY : 0),
      value1: key.scanCode,
    });
  }
}
