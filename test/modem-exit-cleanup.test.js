import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { test } from 'node:test';
import { Line } from '../src/line.js';
import { RtpPortPool } from '../src/media.js';
import { MultiSessionManager } from '../src/session.js';

test('worker exit releases its call after socket closure and cannot terminate another or reused call', async () => {
  const pool = new RtpPortPool({ range: [42000, 42002] });
  const workers = [];
  const closures = [];
  const terminated = [];
  const manager = new MultiSessionManager({
    publicHost: '192.0.2.1', maxSessions: 2, rtpPortPool: pool,
    ppp: { begin: () => ({}), terminate: id => terminated.push(id) },
    modemFactory() {
      const worker = new EventEmitter();
      // A synchronous notification during stop must not re-enter termination.
      worker.stop = () => worker.emit('backend-exit', { code: 0, signal: null });
      workers.push(worker);
      return worker;
    },
    lineFactory(options) {
      const line = new Line(options);
      line.rtpEndpoint.start = async () => {};
      line.rtpEndpoint.stop = () => new Promise(resolve => closures.push(resolve));
      return line;
    }
  });
  const invite = callId => ({ callId, fromTag: 'test', body: [
    'v=0', 'o=ata 1 1 IN IP4 192.0.2.2', 's=-', 'c=IN IP4 192.0.2.2',
    't=0 0', 'm=audio 18000 RTP/AVP 0'
  ].join('\r\n') });
  for (const id of ['failed', 'survivor']) {
    const result = manager.startFromInvite(invite(id));
    assert.equal(await result.ready, true);
    manager.acknowledge(id);
  }
  workers[0].emit('backend-exit', { code: 1, signal: null });
  assert.equal(manager.activeCount, 1);
  assert.deepEqual(terminated, ['failed']);
  assert.equal(pool.available, 0, 'port stays reserved until socket closes');
  assert.equal(manager.sessions.get('survivor').session.state, 'established');
  closures.shift()();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(pool.available, 1);
  const replacement = manager.startFromInvite(invite('failed'));
  assert.equal(await replacement.ready, true);
  workers[0].emit('backend-exit', { code: 1, signal: null });
  assert.equal(manager.activeCount, 2, 'old worker cannot end reused Call-ID');
  for (const id of ['failed', 'survivor']) manager.terminate(id);
  for (const close of closures) close();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(pool.available, 2);
  assert.deepEqual(terminated, ['failed', 'failed', 'survivor']);
});
