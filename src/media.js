import dgram from 'node:dgram';
import { EventEmitter } from 'node:events';
import { spawn } from 'node:child_process';

const DEFAULT_MODEM_FRAME_SAMPLES = 160;
const DEFAULT_ANSWER_TONE_HZ = 2100;
const DEFAULT_V8_REVERSAL_HZ = 15;
const DEFAULT_CARRIER_TONE_HZ = 1800;
const MODEM_FRAME_HEADER_BYTES = 2;
const MAX_MODEM_FRAME_BYTES = 0xffff;

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
      modem: this.modem?.diagnostics ? this.modem.diagnostics() : null,
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

export class InProcessDialupTerminator extends EventEmitter {
  constructor({
    frameSamples = DEFAULT_MODEM_FRAME_SAMPLES,
    answerToneHz = DEFAULT_ANSWER_TONE_HZ,
    answerReversalHz = DEFAULT_V8_REVERSAL_HZ,
    amplitude = 10000,
    intervalMs = 20,
    inboundEnergyThreshold = 400,
    trainingFramesRequired = 3,
    carrierFramesRequired = 6
  } = {}) {
    super();
    this.frameSamples = frameSamples;
    this.answerToneHz = answerToneHz;
    this.answerReversalHz = answerReversalHz;
    this.amplitude = amplitude;
    this.intervalMs = intervalMs;
    this.inboundEnergyThreshold = inboundEnergyThreshold;
    this.trainingFramesRequired = trainingFramesRequired;
    this.carrierFramesRequired = carrierFramesRequired;
    this.codec = null;
    this.sampleOffset = 0;
    this.timer = null;
    this.state = 'idle';
    this.framesIn = 0;
    this.framesOut = 0;
    this.trainingHits = 0;
    this.carrierHits = 0;
    this.stateFramesOut = 0;
    this.lastInboundEnergy = 0;
    this.stateChangedAt = null;
  }

  setSessionCodec(codec) {
    this.codec = codec ?? null;
    this.sampleOffset = 0;
    this.trainingHits = 0;
    this.carrierHits = 0;
    this.stateFramesOut = 0;
    this.lastInboundEnergy = 0;

    if (!this.codec) {
      this.stop();
      this.transition('idle', 'codec-cleared');
      return;
    }

    this.transition('answer-tone', 'codec-selected');
  }

  writeInboundAudio(payload) {
    if (!this.codec) {
      return false;
    }

    if (!this.timer) {
      this.start();
    }

    this.framesIn += 1;
    this.lastInboundEnergy = this.measureEnergy(payload);

    if (this.state === 'answer-tone' && this.lastInboundEnergy >= this.inboundEnergyThreshold) {
      this.trainingHits += 1;
      if (this.trainingHits >= this.trainingFramesRequired) {
        this.transition('v8-training', 'inbound-modem-energy-detected');
      }
    } else if (this.state === 'v8-training' && this.lastInboundEnergy >= this.inboundEnergyThreshold) {
      this.carrierHits += 1;
      if (this.carrierHits >= this.carrierFramesRequired) {
        this.transition('carrier-training', 'sustained-inbound-carrier-detected');
      }
    }

    return true;
  }

  start() {
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
    if (!this.codec || this.state === 'idle') {
      return;
    }

    this.framesOut += 1;
    this.stateFramesOut += 1;

    this.emit('outbound-audio', this.buildNegotiationFrame(), {
      codec: this.codec,
      payloadType: this.codec.payloadType,
      timestampIncrement: this.frameSamples,
      dialupState: this.state
    });
  }

  buildNegotiationFrame() {
    if (this.state === 'carrier-training') {
      return this.buildCarrierTrainingFrame();
    }

    const payload = Buffer.alloc(this.frameSamples);
    for (let index = 0; index < this.frameSamples; index += 1) {
      const absoluteSample = this.sampleOffset + index;
      const phaseReversal = Math.floor((absoluteSample * this.answerReversalHz * 2) / this.codec.clockRate) % 2 === 1;
      const phase = phaseReversal ? Math.PI : 0;
      const sample = Math.round(
        Math.sin((2 * Math.PI * this.answerToneHz * absoluteSample) / this.codec.clockRate + phase) * this.amplitude
      );
      payload[index] = this.codec.payloadType === 8 ? encodeALaw(sample) : encodeMuLaw(sample);
    }
    this.sampleOffset += this.frameSamples;
    return payload;
  }

  buildCarrierTrainingFrame() {
    const payload = Buffer.alloc(this.frameSamples);
    for (let index = 0; index < this.frameSamples; index += 1) {
      const absoluteSample = this.sampleOffset + index;
      const sample = Math.round(
        Math.sin((2 * Math.PI * DEFAULT_CARRIER_TONE_HZ * absoluteSample) / this.codec.clockRate) * this.amplitude
      );
      payload[index] = this.codec.payloadType === 8 ? encodeALaw(sample) : encodeMuLaw(sample);
    }
    this.sampleOffset += this.frameSamples;
    return payload;
  }

  measureEnergy(payload) {
    if (!payload.length) {
      return 0;
    }

    let total = 0;
    for (const encoded of payload) {
      const sample = this.codec.payloadType === 8 ? decodeALaw(encoded) : decodeMuLaw(encoded);
      total += Math.abs(sample);
    }

    return Math.round(total / payload.length);
  }

  transition(nextState, reason) {
    if (this.state === nextState) {
      return;
    }

    const previousState = this.state;
    this.state = nextState;
    this.stateFramesOut = 0;
    this.stateChangedAt = new Date().toISOString();
    this.emit('protocol-state', {
      previousState,
      state: nextState,
      reason,
      at: this.stateChangedAt
    });
    this.emit('backend-log', `dialup protocol ${previousState} -> ${nextState}: ${reason}`);
  }

  diagnostics() {
    return {
      type: 'in-process-dialup-terminator',
      codec: this.codec?.name ?? null,
      state: this.state,
      stateChangedAt: this.stateChangedAt,
      running: Boolean(this.timer),
      framesIn: this.framesIn,
      framesOut: this.framesOut,
      lastInboundEnergy: this.lastInboundEnergy,
      inboundEnergyThreshold: this.inboundEnergyThreshold,
      trainingHits: this.trainingHits,
      trainingFramesRequired: this.trainingFramesRequired,
      carrierHits: this.carrierHits,
      carrierFramesRequired: this.carrierFramesRequired
    };
  }
}

export class ExternalModemProcessBackend extends EventEmitter {
  constructor({
    command,
    args = [],
    env = {},
    restartOnCodecChange = true,
    frameHeaderBytes = MODEM_FRAME_HEADER_BYTES
  } = {}) {
    super();
    if (!command) {
      throw new Error('External modem backend command is required');
    }

    this.command = command;
    this.args = [...args];
    this.env = { ...env };
    this.restartOnCodecChange = restartOnCodecChange;
    this.frameHeaderBytes = frameHeaderBytes;
    this.codec = null;
    this.child = null;
    this.stdoutBuffer = Buffer.alloc(0);
    this.controlBuffer = '';
    this.framesIn = 0;
    this.framesOut = 0;
    this.lastExit = null;
    this.lastError = null;
    this.lastControl = {
      modulation: null,
      baud: null,
      state: 'idle',
      ber: null,
      framesIn: 0,
      framesOut: 0,
      lastEvent: null,
      lastEventAt: null
    };
  }

  setSessionCodec(codec) {
    const nextCodec = codec ?? null;
    const codecChanged = this.codec?.payloadType !== nextCodec?.payloadType;
    this.codec = nextCodec;

    if (!this.codec) {
      this.stop();
      return;
    }

    if (!this.child || (codecChanged && this.restartOnCodecChange)) {
      this.start();
    }
  }

  start() {
    this.stop();
    this.stdoutBuffer = Buffer.alloc(0);
    this.controlBuffer = '';
    this.lastExit = null;

    const child = spawn(this.command, this.args, {
      env: {
        ...process.env,
        ...this.env,
        SIPFAX_MODEM_CODEC: this.codec?.name ?? '',
        SIPFAX_MODEM_PAYLOAD_TYPE: String(this.codec?.payloadType ?? ''),
        SIPFAX_MODEM_CLOCK_RATE: String(this.codec?.clockRate ?? '')
      },
      stdio: ['pipe', 'pipe', 'pipe', 'pipe']
    });

    this.child = child;

    child.stdout.on('data', (chunk) => {
      this.acceptProcessOutput(chunk);
    });
    child.stderr.on('data', (chunk) => {
      this.emit('backend-log', chunk.toString('utf8'));
    });
    child.stdin.on('error', (error) => {
      this.lastError = error.message;
      this.emit('backend-error', error);
    });
    child.stdio[3]?.on('data', (chunk) => {
      this.acceptControlOutput(chunk);
    });
    child.stdio[3]?.on('error', (error) => {
      this.lastError = error.message;
      this.emit('backend-error', error);
    });
    child.on('error', (error) => {
      this.lastError = error.message;
      this.emit('backend-error', error);
    });
    child.on('exit', (code, signal) => {
      this.lastExit = { code, signal };
      if (this.child === child) {
        this.child = null;
      }
      this.emit('backend-exit', this.lastExit);
    });
  }

  writeInboundAudio(payload) {
    if (!this.codec || !this.child || this.child.stdin.destroyed || !this.child.stdin.writable) {
      return false;
    }

    const frame = Buffer.from(payload);
    if (frame.length > MAX_MODEM_FRAME_BYTES) {
      this.emit('backend-error', new Error(`modem frame exceeds ${MAX_MODEM_FRAME_BYTES} bytes`));
      return false;
    }

    const encoded = Buffer.alloc(this.frameHeaderBytes + frame.length);
    encoded.writeUInt16BE(frame.length, 0);
    frame.copy(encoded, this.frameHeaderBytes);
    this.framesIn += 1;
    return this.child.stdin.write(encoded);
  }

  acceptProcessOutput(chunk) {
    this.stdoutBuffer = Buffer.concat([this.stdoutBuffer, chunk]);

    while (this.stdoutBuffer.length >= this.frameHeaderBytes) {
      const frameLength = this.stdoutBuffer.readUInt16BE(0);
      const packetLength = this.frameHeaderBytes + frameLength;
      if (this.stdoutBuffer.length < packetLength) {
        return;
      }

      const frame = this.stdoutBuffer.subarray(this.frameHeaderBytes, packetLength);
      this.stdoutBuffer = this.stdoutBuffer.subarray(packetLength);
      this.framesOut += 1;
      this.emit('outbound-audio', Buffer.from(frame), {
        codec: this.codec,
        payloadType: this.codec?.payloadType ?? null,
        timestampIncrement: frame.length
      });
    }
  }

  acceptControlOutput(chunk) {
    this.controlBuffer += chunk.toString('utf8');

    while (true) {
      const newlineIndex = this.controlBuffer.indexOf('\n');
      if (newlineIndex < 0) {
        return;
      }

      const line = this.controlBuffer.slice(0, newlineIndex).trim();
      this.controlBuffer = this.controlBuffer.slice(newlineIndex + 1);
      if (!line) {
        continue;
      }

      try {
        const event = JSON.parse(line);
        this.lastControl = {
          ...this.lastControl,
          ...event,
          lastEventAt: new Date().toISOString()
        };
        this.emit('backend-control', event);
      } catch (error) {
        this.emit('backend-error', new Error(`invalid modem control JSON: ${error.message}`));
      }
    }
  }

  stop() {
    if (!this.child) {
      return;
    }

    const child = this.child;
    this.child = null;
    child.stdin.end();
    child.kill('SIGTERM');
  }

  diagnostics() {
    return {
      type: 'external-process',
      command: this.command,
      running: Boolean(this.child),
      codec: this.codec?.name ?? null,
      framesIn: this.framesIn,
      framesOut: this.framesOut,
      lastExit: this.lastExit,
      lastError: this.lastError,
      modulation: this.lastControl.modulation,
      baud: this.lastControl.baud,
      state: this.lastControl.state,
      ber: this.lastControl.ber,
      lastEvent: this.lastControl.lastEvent,
      lastEventAt: this.lastControl.lastEventAt
    };
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

export function decodeMuLaw(encoded) {
  const value = (~encoded) & 0xff;
  const sign = value & 0x80;
  const exponent = (value >> 4) & 0x07;
  const mantissa = value & 0x0f;
  const magnitude = ((mantissa << 3) + 0x84) << exponent;
  const sample = magnitude - 0x84;
  return sign ? -sample : sample;
}

export function decodeALaw(encoded) {
  const value = encoded ^ 0x55;
  const sign = value & 0x80;
  const exponent = (value >> 4) & 0x07;
  const mantissa = value & 0x0f;
  const magnitude = exponent === 0
    ? (mantissa << 4) + 8
    : ((mantissa << 4) + 0x108) << (exponent - 1);
  return sign ? magnitude : -magnitude;
}

function randomUInt32() {
  return Math.floor(Math.random() * 0x100000000) >>> 0;
}
