export class LatestFrameRenderer {
  #render;
  #schedule;
  #cancel;
  #requestId = null;
  #frame = null;
  #onPresented = null;
  #disposed = false;

  constructor(render, {
    schedule = (callback) => requestAnimationFrame(callback),
    cancel = (requestId) => cancelAnimationFrame(requestId),
  } = {}) {
    if (typeof render !== 'function') throw new TypeError('render must be a function');
    this.#render = render;
    this.#schedule = schedule;
    this.#cancel = cancel;
  }

  // Takes ownership of frame. Only the newest frame waiting for the next
  // browser presentation opportunity is retained.
  submit(frame, onPresented) {
    if (this.#disposed) {
      frame.close();
      return;
    }

    this.#frame?.close();
    this.#frame = frame;
    this.#onPresented = onPresented ?? null;
    if (this.#requestId !== null) return;

    try {
      this.#requestId = this.#schedule(() => {
        this.#requestId = null;
        const latest = this.#frame;
        const presented = this.#onPresented;
        this.#frame = null;
        this.#onPresented = null;
        if (!latest) return;

        try {
          this.#render(latest);
          presented?.(performance.now());
        } finally {
          latest.close();
        }
      });
    } catch (error) {
      this.#frame = null;
      this.#onPresented = null;
      frame.close();
      throw error;
    }
  }

  dispose() {
    if (this.#disposed) return;
    this.#disposed = true;
    if (this.#requestId !== null) this.#cancel(this.#requestId);
    this.#requestId = null;
    this.#frame?.close();
    this.#frame = null;
    this.#onPresented = null;
  }
}
