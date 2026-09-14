import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtempSync, readFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { AddressPool, PppCredentialStore, PppSessionController } from '../src/ppp.js';
import { PppdSupervisor } from '../src/pppd-supervisor.js';

test('concurrent PPP hooks and teardown cannot affect the surviving call', async () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-concurrent-'));
  const children = [];
  const supervisor = new PppdSupervisor({
    tempDir: root, secretsDir: root, leaseDir: root,
    spawnProcess() {
      const child = new EventEmitter();
      child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
      child.pid = 2000 + children.length;
      child.kill = () => { child.killed = true; return true; };
      children.push(child); return child;
    }
  });
  const pool = new AddressPool({ cidr: '10.83.0.0/29' });
  const controller = new PppSessionController({
    addressPool: pool, pppdSupervisor: supervisor,
    credentials: new PppCredentialStore([{ username: 'test', password: 'test' }])
  });
  function start(id, index) {
    controller.begin(id);
    controller.startPppd(id, { slavePath: `/dev/pts/${index + 1}` });
    const state = supervisor.snapshot(id);
    const descriptor = JSON.parse(readFileSync(state.egressDescriptorPath, 'utf8'));
    const lease = controller.snapshot(id).lease;
    return { state, secretsPath: supervisor.sessions.get(id).secretsPath, token: descriptor.notifyToken, event: {
      callId: id, state: 'ip-up', interfaceName: `ppp${index}`,
      localAddress: lease.localAddress, remoteAddress: lease.clientAddress
    } };
  }
  function exit(index) {
    children[index].exited = true;
    children[index].emit('exit', 0, 'SIGTERM');
  }
  try {
    const a = start('first', 0), b = start('survivor', 1);
    assert.notEqual(a.event.remoteAddress, b.event.remoteAddress);
    assert.notEqual(a.token, b.token);
    assert.notEqual(a.state.egressDescriptorPath, b.state.egressDescriptorPath);
    assert.equal(a.secretsPath, b.secretsPath, 'configured credentials are shared across calls');
    assert.equal(supervisor.acceptHookEvent(a.event, b.token), false);
    assert.equal(supervisor.acceptHookEvent(b.event, a.token), false);
    assert.equal(supervisor.acceptHookEvent(a.event, a.token), true);
    assert.equal(supervisor.acceptHookEvent(b.event, b.token), true);
    const survivor = controller.snapshot('survivor');
    assert.equal(survivor.state, 'ipcp-open');
    assert.equal(pool.leases.size, 2);
    controller.terminate('first');
    assert.equal(children[1].killed, undefined);
    assert.equal(pool.leases.size, 2, 'the first address stays reserved until exit');
    exit(0); await Promise.resolve();
    assert.equal(pool.leases.size, 1);
    assert.deepEqual(controller.snapshot('survivor'), survivor);
    assert.equal(existsSync(b.state.egressDescriptorPath), true);
    assert.equal(existsSync(b.secretsPath), true);
    assert.equal(supervisor.acceptHookEvent({ ...b.event, state: 'ip-down' }, a.token), false);
    assert.equal(supervisor.snapshot('survivor').state, 'ipcp-open');
    const replacement = start('replacement', 2);
    assert.equal(replacement.event.remoteAddress, a.event.remoteAddress);
    assert.equal(supervisor.acceptHookEvent({ ...replacement.event, state: 'ip-down' }, a.token), false);
    assert.deepEqual(controller.snapshot('survivor'), survivor);
    controller.terminate('replacement'); exit(2); await Promise.resolve();
    assert.equal(pool.leases.size, 1);
    assert.equal(supervisor.acceptHookEvent({ ...b.event, state: 'ip-down' }, b.token), true);
    controller.terminate('survivor'); exit(1); await Promise.resolve();
    assert.equal(pool.leases.size, 0);
  } finally {
    for (const [index, child] of children.entries()) if (!child.exited) exit(index);
    rmSync(root, { recursive: true, force: true });
  }
});
