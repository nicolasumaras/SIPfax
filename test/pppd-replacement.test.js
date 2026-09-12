import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { closeSync, mkdtempSync, openSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { PppCredentialStore } from '../src/ppp.js';
import { PppdSupervisor, renderChapSecrets } from '../src/pppd-supervisor.js';

test('credential replacement preserves existing readers and restricts new file permissions', () => {
  const dir = mkdtempSync(join(tmpdir(), 'sipfax-secrets-update-'));
  const path = join(dir, 'chap-secrets');
  writeFileSync(path, 'old complete credentials\n', { mode: 0o644 });
  const reader = openSync(path, 'r');
  try {
    renderChapSecrets(new PppCredentialStore([{ username: 'new', password: 'secret' }]), path);
    assert.equal(readFileSync(reader, 'utf8'), 'old complete credentials\n');
    assert.equal(readFileSync(path, 'utf8'), '"new" * "secret" *\n');
    assert.equal(statSync(path).mode & 0o777, 0o600);
    assert.deepEqual(readdirSync(dir), ['chap-secrets']);
  } finally {
    closeSync(reader);
    rmSync(dir, { recursive: true, force: true });
  }
});

test('late events from a replaced PPP process cannot mutate its successor', () => {
  const dir = mkdtempSync(join(tmpdir(), 'sipfax-replacement-'));
  const children = [];
  const supervisor = new PppdSupervisor({
    secretsDir: dir, tempDir: dir,
    spawnProcess() {
      const child = new EventEmitter();
      child.stdout = new EventEmitter();
      child.stderr = new EventEmitter();
      child.pid = 100 + children.length;
      child.kill = () => { child.killed = true; };
      children.push(child);
      return child;
    }
  });
  const options = {
    callId: 'reused-call', slavePath: '/dev/pts/1',
    lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' },
    credentials: new PppCredentialStore([{ username: 'test', password: 'test' }])
  };
  try {
    supervisor.start(options);
    supervisor.start({ ...options, slavePath: '/dev/pts/2' });
    const expected = supervisor.snapshot(options.callId);
    children[0].stdout.emit('data', Buffer.from('{"state":"ip-down"}\n'));
    children[0].stderr.emit('data', Buffer.from('old process failed'));
    children[0].emit('error', new Error('old process error'));
    children[0].emit('exit', 1, null);
    assert.deepEqual(supervisor.snapshot(options.callId), expected);
    children[1].stdout.emit('data', Buffer.from('{"state":"ip-up","interfaceName":"ppp1"}\n'));
    assert.equal(supervisor.snapshot(options.callId).state, 'ipcp-open');
    children[1].emit('exit', 0, null);
    assert.equal(supervisor.snapshot(options.callId), null);
  } finally {
    supervisor.stop(options.callId);
    rmSync(dir, { recursive: true, force: true });
  }
});
