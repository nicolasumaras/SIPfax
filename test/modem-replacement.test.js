import assert from 'node:assert/strict';
import { once } from 'node:events';
import { test } from 'node:test';
import { ExternalModemProcessBackend, G711_CODECS } from '../src/media.js';

test('replaced modem cannot inject delayed audio, control or exit into the new worker', async () => {
  const modem = new ExternalModemProcessBackend({
    command: process.execPath, args: ['-e', 'setInterval(() => {}, 1000)']
  });
  const events = [];
  for (const name of ['outbound-audio', 'backend-control', 'backend-error', 'backend-exit', 'backend-log']) {
    modem.on(name, () => events.push(name));
  }
  modem.setSessionCodec(G711_CODECS.get(0));
  const old = modem.child;
  await once(old, 'spawn');
  const oldClosed = once(old, 'close');
  modem.start();
  const current = modem.child;
  try {
    await once(current, 'spawn');
    old.stdout.emit('data', Buffer.from([0, 1, 7]));
    old.stdio[3].emit('data', Buffer.from('{"event":"pty-closed"}\n'));
    old.stderr.emit('data', Buffer.from('stale log'));
    old.stdin.emit('error', new Error('stale pipe'));
    old.stdio[3].emit('error', new Error('stale control'));
    await oldClosed;
    assert.deepEqual(events, []);
    assert.equal(modem.child, current);
    assert.equal(modem.lastExit, null);
    current.stdout.emit('data', Buffer.from([0, 1, 8]));
    assert.deepEqual(events, ['outbound-audio']);
  } finally {
    const closed = once(current, 'close');
    modem.stop();
    await closed;
  }
});

test('failed modem spawn never signals the caller process group', async () => {
  const { execFile } = await import('node:child_process');
  const { promisify } = await import('node:util');
  const moduleUrl = new URL('../src/media.js', import.meta.url).href;
  const code = `
    import assert from 'node:assert/strict';
    import { ExternalModemProcessBackend, G711_CODECS } from ${JSON.stringify(moduleUrl)};
    let signals = 0;
    process.on('SIGTERM', () => { signals++; });
    const modem = new ExternalModemProcessBackend({ command: '/nonexistent/sipfax-modem-test' });
    modem.on('backend-error', () => {});
    modem.setSessionCodec(G711_CODECS.get(0));
    assert.ok(!modem.child.pid);
    modem.stop();
    setTimeout(() => { assert.equal(signals, 0); console.log('PASS'); }, 50);
  `;
  // A separate group confines any regression in child.kill after spawn failure.
  const result = await promisify(execFile)(process.execPath, ['--input-type=module', '-e', code],
    { detached: true, timeout: 2000 });
  assert.match(result.stdout, /PASS/);
});
