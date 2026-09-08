// Preserve PCM samples while absorbing short producer/network scheduling bursts.
// No resampling or silence insertion: an underrun is reported and rebuffered.
export class RtpPacer {
  constructor({ delayMs, send, issue = () => {}, now = () => performance.now(),
    schedule = (fn, ms) => setTimeout(fn, ms), cancel = clearTimeout }) {
    Object.assign(this, { delayMs, send, issue, now, schedule, cancel });
    this.queue = [];
    this.timer = null;
    this.deadline = null;
  }

  push(packet, durationMs) {
    if (this.queue.length >= 200) {
      this.issue('pacer-overflow');
      return false;
    }
    this.queue.push({ packet, durationMs });
    if (this.timer === null) {
      this.deadline = this.now() + this.delayMs;
      this.arm();
    }
    return true;
  }

  arm() {
    this.timer = this.schedule(() => this.tick(), Math.max(0, this.deadline - this.now()));
  }

  tick() {
    this.timer = null;
    // Timers may wake slightly early; never send early and accumulate drift.
    if (this.now() < this.deadline) { this.arm(); return; }
    const item = this.queue.shift();
    if (!item) { this.issue('pacer-underrun'); this.deadline = null; return; }
    this.send(item.packet);
    // Do not burst on a delayed event loop. Preserve all queued samples.
    if (this.now() - this.deadline >= item.durationMs) {
      this.issue('pacer-late');
      this.deadline = this.now();
    }
    this.deadline += item.durationMs;
    this.arm();
  }

  reset() {
    if (this.timer !== null) this.cancel(this.timer);
    this.timer = null;
    this.deadline = null;
    this.queue.length = 0;
  }
}
