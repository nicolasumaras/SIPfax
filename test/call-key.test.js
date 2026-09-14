import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { callKey } from '../bin/sipfax-call-key.mjs';
import { PppdSupervisor } from '../src/pppd-supervisor.js';
import { EgressPolicy } from '../src/ppp.js';

test('punctuation, path and long Call-IDs retain separate descriptors and firewall tables', () => {
  const ids = ['a:b', 'a_b', 'a/b', 'b', 'a-b', 'a.b', 'x'.repeat(1000), 'x'.repeat(999) + 'y'];
  const root = mkdtempSync(join(tmpdir(), 'sipfax-call-keys-'));
  try {
    const supervisor = new PppdSupervisor({ leaseDir: root });
    const policy = new EgressPolicy();
    const paths = new Set();
    const tables = new Set();
    for (const callId of ids) {
      const key = callKey(callId);
      assert.match(key, /^c_[a-f0-9]{64}$/);
      const descriptor = policy.leaseDescriptor({ callId, lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' } });
      paths.add(supervisor.writeEgressDescriptor(callId, descriptor));
      tables.add(descriptor.nft.up[0]);
    }
    assert.equal(paths.size, ids.length);
    assert.equal(tables.size, ids.length);
    for (const callId of ids) {
      assert.equal(JSON.parse(readFileSync(join(root, callKey(callId) + '.json'))).callId, callId);
    }
  } finally { rmSync(root, { recursive: true, force: true }); }
});
