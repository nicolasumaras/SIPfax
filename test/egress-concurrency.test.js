import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { promisify } from 'node:util';
import { EgressPolicy } from '../src/ppp.js';
import { callKey } from '../bin/sipfax-call-key.mjs';

test('parallel PPP hooks serialize rules and preserve forwarding for the surviving caller', async () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-egress-parallel-'));
  const mockBin = join(root, 'bin');
  const leases = join(root, 'leases');
  const active = join(root, 'active');
  const log = join(root, 'commands');
  mkdirSync(mockBin); mkdirSync(leases);
  writeFileSync(join(mockBin, 'nft'), '#!/bin/sh\necho BEGIN >> "$SIPFAX_TEST_LOG"\ncat >> "$SIPFAX_TEST_LOG"\nsleep 0.1\necho END >> "$SIPFAX_TEST_LOG"\n', { mode: 0o755 });
  writeFileSync(join(mockBin, 'sysctl'), '#!/bin/sh\necho "$*" >> "$SIPFAX_TEST_LOG"\n', { mode: 0o755 });
  const ids = ['caller/one', 'caller:one'];
  const policy = new EgressPolicy({ operatorUrl: '' });
  for (const [index, callId] of ids.entries()) {
    const descriptor = policy.leaseDescriptor({ callId, lease: { localAddress: '10.64.0.1', clientAddress: `10.64.0.${index + 2}` } });
    writeFileSync(join(leases, callKey(callId) + '.json'), JSON.stringify(descriptor));
  }
  const env = { ...process.env, PATH: mockBin + ':' + process.env.PATH, SIPFAX_PPP_LEASE_DIR: leases, SIPFAX_PPP_ACTIVE_DIR: active, SIPFAX_TEST_LOG: log };
  delete env.SIPFAX_EGRESS_HELD_LOCK;
  const run = (action, id, iface = 'ppp0') => promisify(execFile)(process.execPath, ['bin/sipfax-egress-apply', action, id, iface], { env });
  try {
    await Promise.all(ids.map(id => run('up', id)));
    assert.equal(readdirSync(active).filter(name => name.endsWith('.json')).length, 2);
    const markers = readFileSync(log, 'utf8').split('\n').filter(line => line === 'BEGIN' || line === 'END');
    assert.deepEqual(markers, ['BEGIN', 'END', 'BEGIN', 'END']);
    const beforeDuplicates = readFileSync(log, 'utf8');
    await run('up', ids[0]);
    await run('down', ids[0], 'old-ppp-interface');
    assert.equal(readFileSync(log, 'utf8'), beforeDuplicates);
    await run('down', ids[0]);
    const afterDown = readFileSync(log, 'utf8');
    await run('down', ids[0]);
    assert.equal(readFileSync(log, 'utf8'), afterDown);
    assert.ok(!readFileSync(log, 'utf8').includes('ip_forward=0'));
    assert.equal(readdirSync(active).filter(name => name.endsWith('.json')).length, 1);
    await run('down', ids[1]);
    assert.ok(readFileSync(log, 'utf8').includes('ip_forward=0'));
    assert.equal(readdirSync(active).filter(name => name.endsWith('.json')).length, 0);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
