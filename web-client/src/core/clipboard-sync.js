const defaultPollInterval = 1_000;

export class ClipboardSynchronizer {
  #send;
  #clipboard;
  #document;
  #window;
  #pollInterval;
  #timer = null;
  #started = false;
  #readBlocked = false;
  #readInFlight = false;
  #generation = 0;
  #lastText = null;
  #pendingRemoteText = null;

  constructor(send, options = {}) {
    this.#send = send;
    this.#clipboard = options.clipboard ?? globalThis.navigator?.clipboard;
    this.#document = options.document ?? globalThis.document;
    this.#window = options.window ?? globalThis.window;
    this.#pollInterval = options.pollInterval ?? defaultPollInterval;
  }

  start() {
    if (this.#started || !this.#clipboard) return;
    this.#started = true;
    this.#window.addEventListener('focus', this.#handleOpportunity);
    this.#document.addEventListener('pointerdown', this.#handleInteraction, true);
    if ('onclipboardchange' in this.#clipboard) {
      this.#clipboard.addEventListener('clipboardchange', this.#handleClipboardChange);
    } else {
      this.#timer = this.#window.setInterval(() => {
        if (!this.#readBlocked && this.#document.hasFocus()) void this.syncLocal();
      }, this.#pollInterval);
    }
    void this.syncLocal();
  }

  stop() {
    if (!this.#started) return;
    this.#started = false;
    this.#window.removeEventListener('focus', this.#handleOpportunity);
    this.#document.removeEventListener('pointerdown', this.#handleInteraction, true);
    this.#clipboard?.removeEventListener?.('clipboardchange', this.#handleClipboardChange);
    if (this.#timer !== null) this.#window.clearInterval(this.#timer);
    this.#timer = null;
  }

  async syncLocal() {
    if (!this.#started || this.#readInFlight || this.#pendingRemoteText !== null ||
        !this.#document.hasFocus() || !this.#clipboard?.readText) return false;
    this.#readInFlight = true;
    const generation = this.#generation;
    try {
      const text = await this.#clipboard.readText();
      this.#readBlocked = false;
      if (!this.#started || generation !== this.#generation || text === this.#lastText) return false;
      if (!this.#send(text)) return false;
      this.#lastText = text;
      return true;
    } catch {
      this.#readBlocked = true;
      return false;
    } finally {
      this.#readInFlight = false;
    }
  }

  async receiveRemote(text) {
    if (!this.#started || text === this.#lastText) return false;
    this.#generation += 1;
    this.#pendingRemoteText = text;
    return this.#flushRemote();
  }

  async #flushRemote() {
    const text = this.#pendingRemoteText;
    if (text === null || !this.#document.hasFocus() || !this.#clipboard?.writeText) return false;
    try {
      await this.#clipboard.writeText(text);
      if (this.#pendingRemoteText === text) this.#pendingRemoteText = null;
      this.#lastText = text;
      return true;
    } catch {
      return false;
    }
  }

  #handleClipboardChange = () => {
    if (this.#pendingRemoteText === null) void this.syncLocal();
  };

  #handleOpportunity = () => {
    this.#readBlocked = false;
    if (this.#pendingRemoteText !== null) void this.#flushRemote();
    else void this.syncLocal();
  };

  #handleInteraction = () => {
    this.#handleOpportunity();
  };
}
