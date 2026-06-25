import dgram from 'node:dgram';
import { EventEmitter } from 'node:events';

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
    this.codec = null;
    this.audioBytesIn = 0;
    this.audioBytesOut = 0;
    this.framesIn = 0;
    this.framesOut = 0;
  }

  setSessionCodec(codec) {
    this.codec = codec ?? null;
  }

  attachModem(modem) {
    this.modem = modem;
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
      framesIn: this.framesIn,
      framesOut: this.framesOut,
      audioBytesIn: this.audioBytesIn,
      audioBytesOut: this.audioBytesOut
    };
  }
}

function randomUInt32() {
  return Math.floor(Math.random() * 0x100000000) >>> 0;
}
