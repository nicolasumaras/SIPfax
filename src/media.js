import dgram from 'node:dgram';
import { EventEmitter } from 'node:events';

const DEFAULT_MODEM_FRAME_SAMPLES = 160;
const DEFAULT_ANSWER_TONE_HZ = 2100;

export const G711_CODECS = new Map([
  [0, { payloadType: 0, name: 'PCMU', clockRate: 8000 }],
  [8, { payloadType: 8, name: 'PCMA', clockRate: 8000 }]
]);

export function parseRtpPacket(buffer) {
  if (buffer.length < 12) {
    return null;
  }

  const version = buffer[0] >> 6;
  if (version !== 2) {
    return null;
  }

  const csrcCount = buffer[0] & 0x0f;
  const extension = (buffer[0] & 0x10) !== 0;
  let headerLength = 12 + csrcCount * 4;

  if (buffer.length < headerLength) {
    return null;
  }

  if (extension) {
    if (buffer.length < headerLength + 4) {
      return null;
    }
    const extensionWords = buffer.readUInt16BE(headerLength + 2);
    headerLength += 4 + extensionWords * 4;
  }

  if (buffer.length < headerLength) {
    return null;
  }

  return {
    payloadType: buffer[1] & 0x7f,
    sequenceNumber: buffer.readUInt16BE(2),
    timestamp: buffer.readUInt32BE(4),
    ssrc: buffer.readUInt32BE(8),
    payload: buffer.subarray(headerLength)
  };
}

export function buildRtpPacket({ payloadType, sequenceNumber, timestamp, ssrc, payload, marker = false }) {
  const audio = Buffer.from(payload);
  const packet = Buffer.alloc(12 + audio.length);

  packet[0] = 0x80;
  packet[1] = (marker ? 0x80 : 0) | (payloadType & 0x7f);
  packet.writeUInt16BE(sequenceNumber & 0xffff, 2);
  packet.writeUInt32BE(timestamp >>> 0, 4);
  packet.writeUInt32BE(ssrc >>> 0, 8);
  audio.copy(packet, 12);

  return packet;
}

export class RtpEndpoint extends EventEmitter {
  constructor({ host, port, ssrc = randomUInt32() }) {
    super();
    this.host = host;
    this.port = port;
    this.socket = dgram.createSocket('udp4');
    this.expectedPayloadType = null;
    this.remote = null;
    this.ssrc = ssrc;
    this.outboundSequenceNumber = 0;
    this.outboundTimestamp = 0;

    this.socket.on('message', (message, remote) => {
      const packet = parseRtpPacket(message);
      if (!packet) {
        this.emit('dropped', { reason: 'invalid-rtp', remote });
        return;
      }

      if (this.expectedPayloadType !== null && packet.payloadType !== this.expectedPayloadType) {
        this.emit('dropped', {
          reason: 'unexpected-payload-type',
          payloadType: packet.payloadType,
          expectedPayloadType: this.expectedPayloadType,
          remote
        });
        return;
      }

      this.remote = { address: remote.address, port: remote.port };
      this.emit('frame', { ...packet, remote: this.remote });
    });
  }

  setSessionCodec(codec) {
    this.expectedPayloadType = codec?.payloadType ?? null;
    this.outboundSequenceNumber = 0;
    this.outboundTimestamp = 0;
  }

  sendPayload(payload, { payloadType = this.expectedPayloadType, timestampIncrement = payload.length, marker = false } = {}) {
    if (!this.remote || payloadType === null || payloadType === undefined) {
      return false;
    }

    const packet = buildRtpPacket({
      payloadType,
      sequenceNumber: this.outboundSequenceNumber,
      timestamp: this.outboundTimestamp,
      ssrc: this.ssrc,
      payload,
      marker
    });

    this.outboundSequenceNumber = (this.outboundSequenceNumber + 1) & 0xffff;
    this.outboundTimestamp = (this.outboundTimestamp + timestampIncrement) >>> 0;
    this.socket.send(packet, this.remote.port, this.remote.address);
    return true;
  }

  start() {
    return new Promise((resolve) => {
      this.socket.bind(this.port, this.host, resolve);
    });
  }

  stop() {
    return new Promise((resolve, reject) => {
      this.socket.close((error) => {
        if (error) {
          reject(error);
          return;
        }
        resolve();
      });
    });
  }
}

export class ModemBridge extends EventEmitter {
  constructor({ modem = null } = {}) {
    super();
    this.modem = modem;
    this.modemOutboundHandler = null;
    this.codec = null;
    this.audioBytesIn = 0;
    this.audioBytesOut = 0;
    this.framesIn = 0;
    this.framesOut = 0;
    this.attachModem(modem);
  }

  setSessionCodec(codec) {
    this.codec = codec ?? null;
    if (this.modem?.setSessionCodec) {
      this.modem.setSessionCodec(this.codec);
    }
  }

  attachModem(modem) {
    if (this.modem?.off && this.modemOutboundHandler) {
      this.modem.off('outbound-audio', this.modemOutboundHandler);
    }

    this.modem = modem;
    this.modemOutboundHandler = null;

    if (!modem) {
      return;
    }

    if (modem.setSessionCodec) {
      modem.setSessionCodec(this.codec);
    }

    if (modem.on) {
      this.modemOutboundHandler = (payload, metadata = {}) => {
        this.acceptOutboundAudio(payload, metadata);
      };
      modem.on('outbound-audio', this.modemOutboundHandler);
    }
  }

  acceptFrame(frame) {
    const payload = Buffer.from(frame.payload);
    const audio = {
      codec: this.codec,
      payloadType: frame.payloadType,
      sequenceNumber: frame.sequenceNumber,
      timestamp: frame.timestamp,
      payload
    };

    this.framesIn += 1;
    this.audioBytesIn += payload.length;
    this.emit('inbound-audio', audio);

    if (this.modem?.writeInboundAudio) {
      this.modem.writeInboundAudio(payload, {
        codec: this.codec,
        payloadType: frame.payloadType,
        sequenceNumber: frame.sequenceNumber,
        timestamp: frame.timestamp
      });
    }
  }

  acceptOutboundAudio(payload, metadata = {}) {
    const audioPayload = Buffer.from(payload);
    const audio = {
      codec: metadata.codec ?? this.codec,
      payloadType: metadata.payloadType ?? metadata.codec?.payloadType ?? this.codec?.payloadType ?? null,
      timestampIncrement: metadata.timestampIncrement ?? audioPayload.length,
      marker: metadata.marker ?? false,
      payload: audioPayload
    };

    this.framesOut += 1;
    this.audioBytesOut += audioPayload.length;
    this.emit('outbound-audio', audio);
    return audio;
  }

  diagnostics() {
    return {
      codec: this.codec?.name ?? null,
      modemAttached: Boolean(this.modem),
      framesIn: this.framesIn,
      framesOut: this.framesOut,
      audioBytesIn: this.audioBytesIn,
      audioBytesOut: this.audioBytesOut
    };
  }
}

export class ModemAnswerToneSource extends EventEmitter {
  constructor({
    frameSamples = DEFAULT_MODEM_FRAME_SAMPLES,
    toneHz = DEFAULT_ANSWER_TONE_HZ,
    amplitude = 10000,
    intervalMs = 20
  } = {}) {
    super();
    this.frameSamples = frameSamples;
    this.toneHz = toneHz;
    this.amplitude = amplitude;
    this.intervalMs = intervalMs;
    this.codec = null;
    this.sampleOffset = 0;
    this.timer = null;
  }

  setSessionCodec(codec) {
    this.codec = codec ?? null;
    this.sampleOffset = 0;
    if (!this.codec) {
      this.stop();
    }
  }

  writeInboundAudio() {
    if (!this.codec || this.timer) {
      return;
    }

    this.emitFrame();
    this.timer = setInterval(() => {
      this.emitFrame();
    }, this.intervalMs);
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  emitFrame() {
    if (!this.codec) {
      return;
    }

    this.emit('outbound-audio', this.buildToneFrame(), {
      codec: this.codec,
      payloadType: this.codec.payloadType,
      timestampIncrement: this.frameSamples
    });
  }

  buildToneFrame() {
    const payload = Buffer.alloc(this.frameSamples);
    for (let index = 0; index < this.frameSamples; index += 1) {
      const sample = Math.round(
        Math.sin((2 * Math.PI * this.toneHz * (this.sampleOffset + index)) / this.codec.clockRate) * this.amplitude
      );
      payload[index] = this.codec.payloadType === 8 ? encodeALaw(sample) : encodeMuLaw(sample);
    }
    this.sampleOffset += this.frameSamples;
    return payload;
  }
}

export function encodeMuLaw(sample) {
  const clipped = Math.max(-32635, Math.min(32635, sample));
  const sign = clipped < 0 ? 0x80 : 0x00;
  let magnitude = Math.abs(clipped) + 0x84;
  let exponent = 7;

  for (let mask = 0x4000; exponent > 0 && (magnitude & mask) === 0; mask >>= 1) {
    exponent -= 1;
  }

  const mantissa = (magnitude >> (exponent + 3)) & 0x0f;
  return (~(sign | (exponent << 4) | mantissa)) & 0xff;
}

export function encodeALaw(sample) {
  const clipped = Math.max(-32768, Math.min(32767, sample));
  const sign = clipped < 0 ? 0x00 : 0x80;
  let magnitude = Math.abs(clipped);

  if (magnitude > 0x7ff) {
    let exponent = 7;
    for (let mask = 0x4000; exponent > 0 && (magnitude & mask) === 0; mask >>= 1) {
      exponent -= 1;
    }
    magnitude = ((exponent << 4) | ((magnitude >> (exponent + 3)) & 0x0f)) & 0x7f;
  } else {
    magnitude = (magnitude >> 4) & 0x7f;
  }

  return (sign | magnitude) ^ 0x55;
}

function randomUInt32() {
  return Math.floor(Math.random() * 0x100000000) >>> 0;
}
