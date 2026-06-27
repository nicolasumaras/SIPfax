import { EventEmitter } from 'node:events';
import { ModemBridge, RtpEndpoint } from './media.js';

/**
 * One concurrent modem call: its own RTP endpoint (on an allocated port), modem
 * bridge, and modem backend process, wired together. Emits pty-opened/pty-closed
 * (carrying the callId) so the session manager can attach/detach pppd.
 */
export class Line extends EventEmitter {
  constructor({ callId, codec, rtpHost, rtpPort, modem }) {
    super();
    this.callId = callId;
    this.codec = codec;
    this.rtpPort = rtpPort;
    this.modem = modem;
    this.rtpEndpoint = new RtpEndpoint({ host: rtpHost, port: rtpPort });
    this.modemBridge = new ModemBridge({ modem });
    this.lastControl = {};
    this.metrics = { rtpFramesAccepted: 0, rtpFramesDropped: 0 };

    this.rtpEndpoint.on('frame', (frame) => {
      this.metrics.rtpFramesAccepted += 1;
      this.modemBridge.acceptFrame(frame);
    });
    this.rtpEndpoint.on('dropped', () => {
      this.metrics.rtpFramesDropped += 1;
    });
    this.modemBridge.on('outbound-audio', (audio) => {
      this.rtpEndpoint.sendPayload(audio.payload, {
        payloadType: audio.payloadType,
        timestampIncrement: audio.timestampIncrement,
        marker: audio.marker
      });
    });

    if (modem?.on) {
      modem.on('backend-log', (line) => this.emit('backend-log', { callId, line }));
      modem.on('backend-error', (error) => this.emit('backend-error', { callId, error }));
      modem.on('backend-control', (event) => this.#handleModemControl(event));
    }

    this.rtpEndpoint.setSessionCodec(codec);
    this.modemBridge.setSessionCodec(codec);
  }

  #handleModemControl(event) {
    this.lastControl = { ...this.lastControl, ...event };
    const name = event.event ?? event.lastEvent ?? event.state;
    if (name === 'pty-opened') {
      const slavePath = event.slavePath ?? event.ptySlavePath ?? event.ptyPath;
      this.emit('pty-opened', { callId: this.callId, slavePath });
    } else if (name === 'pty-closed') {
      this.emit('pty-closed', { callId: this.callId });
    }
  }

  start() {
    return this.rtpEndpoint.start();
  }

  async stop() {
    try { this.modem?.stop?.(); } catch { /* best-effort teardown */ }
    try { await this.rtpEndpoint.stop(); } catch { /* socket may already be closed */ }
  }

  diagnostics() {
    const modem = this.modem?.diagnostics?.() ?? {};
    return {
      callId: this.callId,
      rtpPort: this.rtpPort,
      codec: this.codec?.name ?? null,
      modulation: modem.modulation ?? null,
      baud: modem.baud ?? null,
      state: modem.state ?? null,
      lastEvent: modem.lastEvent ?? this.lastControl.event ?? null,
      metrics: { ...this.metrics }
    };
  }
}
