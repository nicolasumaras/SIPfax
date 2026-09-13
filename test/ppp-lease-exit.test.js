import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { AddressPool, PppCredentialStore, PppSessionController } from '../src/ppp.js';
import { PppdSupervisor } from '../src/pppd-supervisor.js';

test('terminating PPP retains its address until the child exits', async () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-lease-exit-'));
  const children = [];
  const supervisor = new PppdSupervisor({
    tempDir: root, secretsDir: root, leaseDir: root,
    spawnProcess() {
      const child = new EventEmitter();
      child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
      child.pid = 1000 + children.length;
      child.kill = () => { child.killed = true; return true; };
      children.push(child); return child;
    }
  });
  const pool = new AddressPool({ cidr: '10.81.0.0/30' });
  const controller = new PppSessionController({
    addressPool: pool, pppdSupervisor: supervisor,
    credentials: new PppCredentialStore([{ username: 'test', password: 'test' }])
  });
  try {
    controller.begin('first');
    controller.startPppd('first', { slavePath: '/dev/pts/1' });
    const address = controller.snapshot('first').lease.clientAddress;
    controller.terminate('first');
    assert.equal(pool.leases.size, 1, 'SIGTERM is not process exit');
    assert.throws(() => controller.begin('first'), /terminating/);
    controller.begin('second');
    assert.throws(() => controller.startPppd('second', { slavePath: '/dev/pts/2' }));
    children[0].emit('exit', 0, 'SIGTERM');
    await Promise.resolve();
    assert.equal(pool.leases.size, 0);
    controller.startPppd('second', { slavePath: '/dev/pts/2' });
    assert.equal(controller.snapshot('second').lease.clientAddress, address);
    children[0].emit('exit', 0, 'SIGTERM');
    await Promise.resolve();
    assert.equal(pool.leases.size, 1, 'old exit cannot release a new lease');
    controller.stopPppd('second'); // PTY closure may precede SIP teardown.
    controller.terminate('second');
    assert.equal(pool.leases.size, 1);
    children[1].emit('exit', 0, 'SIGTERM');
    await Promise.resolve();
    assert.equal(pool.leases.size, 0);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test('failed pppd spawn releases a terminating lease without an exit event', { timeout: 3000 }, async () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-lease-spawn-'));
  const supervisor = new PppdSupervisor({
    command: join(root, 'missing-pppd'), tempDir: root, secretsDir: root, leaseDir: root
  });
  const pool = new AddressPool({ cidr: '10.82.0.0/30' });
  const controller = new PppSessionController({
    addressPool: pool, pppdSupervisor: supervisor,
    credentials: new PppCredentialStore([{ username: 'test', password: 'test' }])
  });
  try {
    controller.begin('failed-spawn');
    controller.startPppd('failed-spawn', { slavePath: '/dev/pts/1' });
    const exited = supervisor.whenExited('failed-spawn');
    controller.terminate('failed-spawn');
    await exited;
    await Promise.resolve();
    assert.equal(pool.leases.size, 0);
    assert.doesNotThrow(() => controller.begin('failed-spawn'));
  } finally { rmSync(root, { recursive: true, force: true }); }
});
