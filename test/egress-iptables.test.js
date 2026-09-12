import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { promisify } from 'node:util';
import { EgressPolicy } from '../src/ppp.js';
import { callKey } from '../bin/sipfax-call-key.mjs';

test('partial iptables installation removes only newly installed rules', async () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-iptables-failure-'));
  const mockBin = join(root, 'bin');
  const leases = join(root, 'leases');
  const active = join(root, 'active');
  const stateFile = join(root, 'state');
  const sysctlLog = join(root, 'sysctl');
  mkdirSync(mockBin); mkdirSync(leases);
  writeFileSync(stateFile, JSON.stringify({ rules: ['unrelated existing rule'], additions: 0, trace: [] }));
  const fake = `#!${process.execPath}
const fs = require('node:fs');
const path = process.env.SIPFAX_TEST_STATE;
const state = JSON.parse(fs.readFileSync(path));
const args = process.argv.slice(2);
const pos = args.findIndex(arg => arg === '-A' || arg === '-D');
const op = args.splice(pos, 1)[0];
const rule = JSON.stringify(args);
state.trace.push([op, rule]);
if (op === '-A') {
  state.additions++;
  if (state.additions === 4) { fs.writeFileSync(path, JSON.stringify(state)); process.exit(1); }
  state.rules.push(rule);
} else {
  const index = state.rules.indexOf(rule);
  if (index < 0) process.exit(2);
  state.rules.splice(index, 1);
}
fs.writeFileSync(path, JSON.stringify(state));
`;
  for (const name of ['iptables', 'iptables-nft']) writeFileSync(join(mockBin, name), fake, { mode: 0o755 });
  writeFileSync(join(mockBin, 'sysctl'), '#!/bin/sh\nif test "$1" = -n; then echo 0; else echo "$*" >> "$SIPFAX_TEST_SYSCTL"; fi\n', { mode: 0o755 });
  const callId = 'iptables-failure';
  const descriptor = new EgressPolicy({ operatorUrl: '' }).leaseDescriptor({
    callId, lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' }
  });
  descriptor.nft = {}; // Exercise the fallback even on hosts with nft installed.
  writeFileSync(join(leases, callKey(callId) + '.json'), JSON.stringify(descriptor));
  const env = { ...process.env, PATH: mockBin + ':' + process.env.PATH,
    SIPFAX_PPP_LEASE_DIR: leases, SIPFAX_PPP_ACTIVE_DIR: active,
    SIPFAX_TEST_STATE: stateFile, SIPFAX_TEST_SYSCTL: sysctlLog };
  delete env.SIPFAX_EGRESS_HELD_LOCK;
  const run = action => promisify(execFile)(process.execPath,
    ['bin/sipfax-egress-apply', action, callId, 'ppp0'], { env });
  try {
    await assert.rejects(run('up'));
    const failed = JSON.parse(readFileSync(stateFile, 'utf8'));
    assert.deepEqual(failed.rules, ['unrelated existing rule']);
    assert.deepEqual(failed.trace.slice(4), failed.trace.slice(0, 3).reverse().map(([, rule]) => ['-D', rule]));
    assert.ok(!readdirSync(root).includes('sysctl'));
    assert.deepEqual(readdirSync(active).filter(name => name.endsWith('.json')), []);
    // Retry after the one injected failure, then exercise complete teardown.
    await run('up');
    assert.equal(readdirSync(active).filter(name => name.endsWith('.json')).length, 1);
    await run('down');
    assert.deepEqual(JSON.parse(readFileSync(stateFile, 'utf8')).rules, ['unrelated existing rule']);
    assert.deepEqual(readdirSync(active).filter(name => name.endsWith('.json')), []);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
