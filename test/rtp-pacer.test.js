import { test } from 'node:test';
import assert from 'node:assert/strict';
import { RtpPacer } from '../src/rtp-pacer.js';

function harness() {
  let time = 0, id = 0;
  const timers = new Map(), sent = [], issues = [];
  const pacer = new RtpPacer({ delayMs: 60, now: () => time,
    schedule: (fn, ms) => { timers.set(++id, { fn, at: time + ms }); return id; },
    cancel: (id) => timers.delete(id),
    send: (packet) => sent.push({ time, packet }), issue: (reason) => issues.push(reason) });
  function advance(to) {
    for (;;) {
      const next = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0];
      if (!next || next[1].at > to) break;
      time = next[1].at; timers.delete(next[0]); next[1].fn();
    }
    time = to;
  }
  return { pacer, sent, issues, advance, timers };
}

test('paces a 56ms arrival gap and catch-up burst without altering or losing payloads', () => {
  const h = harness();
  const arrivals = [0, 20, 40, 96, 97, 100, 120, 140];
  arrivals.forEach((time, i) => { h.advance(time); h.pacer.push(Buffer.from([i, 0, 255]), 20); });
  h.advance(201);
  assert.deepEqual(h.sent.map(p => p.time), [60, 80, 100, 120, 140, 160, 180, 200]);
  assert.deepEqual(h.sent.map(p => [...p.packet]), arrivals.map((_, i) => [i, 0, 255]));
  assert.deepEqual(h.issues, []);
  h.pacer.reset(); assert.equal(h.timers.size, 0);
});

test('reports underrun, resumes promptly and bounds the queue', () => {
  const h = harness(); h.pacer.push('one', 20); h.advance(90);
  assert.deepEqual(h.issues, ['pacer-underrun']);
  h.pacer.push('two', 20); h.advance(150);
  assert.equal(h.sent[1].time, 90);
  h.pacer.reset();
  for (let i = 0; i < 200; i++) assert.equal(h.pacer.push(i, 20), true);
  assert.equal(h.pacer.push(201, 20), false);
  assert.equal(h.issues.at(-1), 'pacer-overflow');
  h.pacer.reset(); h.advance(1000); assert.equal(h.sent.length, 2);
});

test('a brief shortage does not add another startup delay or burst recovered packets', () => {
  const h = harness();
  h.pacer.push('first', 20); h.advance(80);
  h.advance(86.8);
  h.pacer.push('second', 20); h.pacer.push('third', 20);
  h.advance(107);
  assert.deepEqual(h.sent, [
    { time: 60, packet: 'first' },
    { time: 86.8, packet: 'second' },
    { time: 106.8, packet: 'third' }
  ]);
  h.pacer.reset(); h.pacer.push('new session', 20); h.advance(167);
  assert.equal(h.sent.at(-1).time, 167);
});
