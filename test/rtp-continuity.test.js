import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ModemBridge } from '../src/media.js';
import { RtpContinuity } from '../src/rtp-continuity.js';
const frame = (sequenceNumber, timestamp, extra = {}) => ({ sequenceNumber, timestamp, ssrc: 1, payloadType: 0, payload: Buffer.alloc(160, 0x81), ...extra });

test('missing RTP input advances the modem clock without changing real audio', () => {
  const writes = [], events = [];
  const bridge = new ModemBridge({ modem: { writeInboundAudio: p => writes.push(p) } });
  bridge.on('timing', e => events.push(e));
  bridge.acceptFrame(frame(10, 0)); bridge.acceptFrame(frame(12, 320));
  assert.equal(writes.reduce((n, p) => n + p.length, 0), 480);
  assert.deepEqual(writes, [Buffer.alloc(160, 0x81), Buffer.alloc(160, 0xff), Buffer.alloc(160, 0x81)]);
  assert.deepEqual(events, [{ reason: 'rtp-input-erasure', samples: 160 }]);
});

test('late and duplicate packets cannot replay samples after gap repair', () => {
  const c = new RtpContinuity();
  c.accept(frame(10, 0)); c.accept(frame(12, 320));
  assert.deepEqual(c.accept(frame(11, 160)), []);
  assert.deepEqual(c.accept(frame(12, 320)), []);
  assert.equal(c.accept(frame(13, 480)).length, 1);
});

test('sample gaps handle timestamp and sequence wrap and A-law silence', () => {
  const c = new RtpContinuity();
  c.accept(frame(65535, 0xffffff60, { payloadType: 8 }));
  const out = c.accept(frame(1, 160, { payloadType: 8 }));
  assert.equal(out.length, 2);
  assert.equal(out[0].timestamp, 0);
  assert.deepEqual(out[0].payload, Buffer.alloc(160, 0xd5));
});

test('source changes, reset and large discontinuities never fabricate long gaps', () => {
  const c = new RtpContinuity();
  c.accept(frame(10, 0));
  assert.equal(c.accept(frame(1, 90000, { ssrc: 2 })).length, 1);
  assert.equal(c.accept(frame(2, 800000)).length, 1);
  c.reset(); assert.equal(c.accept(frame(1, 0)).length, 1);
});

test('timestamp duration rather than packet count controls variable-sized erasures', () => {
  const c = new RtpContinuity();
  c.accept(frame(1, 0, { payload: Buffer.alloc(80) }));
  const out = c.accept(frame(3, 280));
  assert.deepEqual(out.map(p => p.payload.length), [160, 40, 160]);
});

test('a startup timestamp overlap preserves real audio and establishes a new baseline', () => {
  const events = [], c = new RtpContinuity({ report: e => events.push(e) });
  c.accept(frame(1, 0));
  const next = frame(2, 120);
  assert.deepEqual(c.accept(next), [next]);
  assert.equal(events[0].reason, 'rtp-timestamp-discontinuity');
  assert.equal(c.accept(frame(3, 280)).length, 1);
});
