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

export class RtpEndpoint extends EventEmitter {
  constructor({ host, port }) {
    super();
    this.host = host;
    this.port = port;
    this.socket = dgram.createSocket('udp4');
    this.expectedPayloadType = null;
    this.remote = null;

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

export class ModemBridge {
  constructor() {
    this.frames = [];
  }

  acceptFrame(frame) {
    this.frames.push({
      payloadType: frame.payloadType,
      sequenceNumber: frame.sequenceNumber,
      timestamp: frame.timestamp,
      byteLength: frame.payload.length
    });
  }
}
