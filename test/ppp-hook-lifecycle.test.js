import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { OperatorHttpServer } from '../src/operator.js';
import { PppdSupervisor } from '../src/pppd-supervisor.js';
import { PppCredentialStore } from '../src/ppp.js';

test('authenticated hook events update only their current PPP process', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'sipfax-hook-lifecycle-'));
  const children = [], events = [];
  const supervisor = new PppdSupervisor({
    secretsDir: dir, tempDir: dir, leaseDir: dir,
    spawnProcess() {
      const child = new EventEmitter();
      child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
      child.pid = 100 + children.length; child.kill = () => {};
      children.push(child); return child;
    }
  });
  const operator = new OperatorHttpServer({
    host: '127.0.0.1', port: 0, diagnostics: () => ({}),
    onPppEvent: (event, token) => supervisor.acceptHookEvent(event, token)
  });
  const options = {
    callId: 'reused-hook-call', slavePath: '/dev/pts/1',
    lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' },
    credentials: new PppCredentialStore([{ username: 'test', password: 'test' }]),
    egressDescriptor: { callId: 'reused-hook-call' }, onEvent: event => events.push(event)
  };
  function start() {
    const session = supervisor.start(options);
    const descriptor = JSON.parse(readFileSync(session.egressDescriptorPath, 'utf8'));
    assert.equal(JSON.stringify(supervisor.diagnostics()).includes(descriptor.notifyToken), false);
    return descriptor.notifyToken;
  }
  await operator.start();
  const url = `http://127.0.0.1:${operator.server.address().port}/ppp/events`;
  const event = { callId: options.callId, state: 'ip-up', interfaceName: 'ppp0',
    localAddress: '10.64.0.1', remoteAddress: '10.64.0.2' };
  async function post(token, body = event) {
    const response = await fetch(url, { method: 'POST',
      headers: { 'content-type': 'application/json', ...(token ? { 'x-sipfax-notify-token': token } : {}) },
      body: JSON.stringify(body) });
    await response.arrayBuffer(); return response.status;
  }
  try {
    const token = start();
    assert.equal(await post(null), 403);
    assert.equal(await post('0'.repeat(64)), 403);
    assert.equal(await post(token, { ...event, remoteAddress: '10.64.0.3' }), 403);
    assert.equal(supervisor.snapshot(options.callId).state, 'starting');
    assert.equal(await post(token), 202);
    assert.equal(supervisor.snapshot(options.callId).state, 'ipcp-open');
    assert.equal(events.at(-1).interfaceName, 'ppp0');
    const replacementToken = start();
    assert.notEqual(replacementToken, token);
    assert.equal(await post(token, { ...event, state: 'ip-down' }), 403);
    assert.equal(supervisor.snapshot(options.callId).state, 'starting');
    assert.equal(await post(replacementToken), 202);
    assert.equal(await post(replacementToken, { ...event, state: 'ip-down' }), 202);
    assert.equal(supervisor.snapshot(options.callId).state, 'ipcp-closed');
    const history = await fetch(url).then(r => r.json());
    assert.equal(history.events.length, 3);
    assert.equal(JSON.stringify(history).includes(replacementToken), false);
  } finally {
    for (const child of children) child.emit('exit', 0, null);
    supervisor.stop(options.callId);
    await operator.stop();
    rmSync(dir, { recursive: true, force: true });
  }
});
