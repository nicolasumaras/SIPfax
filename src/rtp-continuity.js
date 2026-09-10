// G.711 has one sample per RTP timestamp unit. Silence marks an erasure;
// it does not reconstruct modem data. Keep bounded gaps on the sample timeline.
export class RtpContinuity {
  constructor({ report = () => {} } = {}) { this.report = report; this.reset(); }
  reset() { this.previous = null; }

  accept(frame) {
    const { payloadType, sequenceNumber, timestamp, ssrc, payload } = frame;
    if (![0, 8].includes(payloadType) || ![sequenceNumber, timestamp, ssrc].every(Number.isInteger)) {
      this.reset(); return [frame];
    }
    const previous = this.previous;
    const result = [];
    if (previous && previous.ssrc === ssrc && previous.payloadType === payloadType) {
      const advance = (sequenceNumber - previous.sequenceNumber) & 0xffff;
      if (!advance || advance >= 0x8000) {
        this.report({ reason: 'rtp-late-or-duplicate', samples: payload.length });
        return [];
      }
      const missing = (timestamp - previous.end) >>> 0;
      if (missing > 0 && missing <= 8000) {
        this.report({ reason: 'rtp-input-erasure', samples: missing });
        for (let offset = 0; offset < missing; offset += 160) {
          result.push({ ...frame, timestamp: (previous.end + offset) >>> 0,
            payload: Buffer.alloc(Math.min(160, missing - offset), payloadType === 0 ? 0xff : 0xd5),
            erasure: true });
        }
      } else if (missing) {
        // A timestamp restart or a long outage must not allocate unbounded audio.
        this.report({ reason: 'rtp-timestamp-discontinuity', samples: missing });
      }
    }
    this.previous = { ssrc, payloadType, sequenceNumber, end: (timestamp + payload.length) >>> 0 };
    result.push(frame);
    return result;
  }
}
