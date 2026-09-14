import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { promisify } from 'node:util';
import { EgressPolicy } from '../src/ppp.js';
import { callKey } from '../bin/sipfax-call-key.mjs';

for (const globalValue of ['0', '1']) {
  test(`last PPP caller restores the original forwarding baseline (${globalValue})`, async () => {
    const root = mkdtempSync(join(tmpdir(), 'sipfax-forwarding-baseline-'));
    const mockBin = join(root, 'bin');
    const leases = join(root, 'leases');
    const active = join(root, 'active');
    const state = join(root, 'state');
    const log = join(root, 'rules-log');
    const fail = join(root, 'fail-once');
    mkdirSync(mockBin); mkdirSync(leases);
    const initial = {
      'net.ipv4.ip_forward': globalValue,
      'net.ipv4.conf.all.forwarding': globalValue,
      'net.ipv4.conf.default.forwarding': globalValue,
      'net.ipv4.conf.eth0.forwarding': '0',
      'net.ipv4.conf.eth1.forwarding': '1',
      'net.ipv4.conf.unrelated.forwarding': '0'
    };
    writeFileSync(state, JSON.stringify(initial));
    writeFileSync(join(mockBin, 'sysctl'), `#!${process.execPath}
const fs = require('node:fs');
const values = JSON.parse(fs.readFileSync(process.env.SIPFAX_TEST_STATE));
const [mode, arg] = process.argv.slice(2);
if (mode === '-n') { if (!(arg in values)) process.exit(1); console.log(values[arg]); process.exit(0); }
if (mode === '-a') {
  for (const [key, value] of Object.entries(values)) if (key.startsWith('net.ipv4.conf.')) console.log(key + ' = ' + value);
  process.exit(0);
}
const [key, value] = arg.split('=');
if (key === 'net.ipv4.conf.eth1.forwarding' && value === '1' && fs.existsSync(process.env.SIPFAX_TEST_FAIL)) {
  fs.unlinkSync(process.env.SIPFAX_TEST_FAIL); process.exit(1);
}
values[key] = value;
// Model the global switch resetting existing interface forwarding values.
if (key === 'net.ipv4.ip_forward') for (const k of Object.keys(values)) if (k.startsWith('net.ipv4.conf.')) values[k] = value;
fs.writeFileSync(process.env.SIPFAX_TEST_STATE, JSON.stringify(values));
`, { mode: 0o755 });
    writeFileSync(join(mockBin, 'nft'), '#!/bin/sh\ncat >> "$SIPFAX_TEST_LOG"\n', { mode: 0o755 });
    const ids = ['first', 'second'];
    for (const [i, callId] of ids.entries()) {
      const descriptor = new EgressPolicy({ operatorUrl: '', outboundInterface: `eth${i}` }).leaseDescriptor({
        callId, lease: { localAddress: '10.64.0.1', clientAddress: `10.64.0.${i + 2}` }
      });
      writeFileSync(join(leases, callKey(callId) + '.json'), JSON.stringify(descriptor));
    }
    const env = { ...process.env, PATH: mockBin + ':' + process.env.PATH,
      SIPFAX_PPP_LEASE_DIR: leases, SIPFAX_PPP_ACTIVE_DIR: active,
      SIPFAX_TEST_STATE: state, SIPFAX_TEST_LOG: log, SIPFAX_TEST_FAIL: fail };
    delete env.SIPFAX_EGRESS_HELD_LOCK;
    const run = (action, id) => promisify(execFile)(process.execPath,
      ['bin/sipfax-egress-apply', action, id, id === 'first' ? 'ppp0' : 'ppp1'], { env });
    try {
      await run('up', 'first');
      await run('up', 'second');
      await run('down', 'first');
      assert.equal(JSON.parse(readFileSync(state))['net.ipv4.ip_forward'], '1');
      assert.ok(existsSync(join(active, '.forwarding-state')));
      if (globalValue === '0') {
        writeFileSync(fail, 'fail restoration once');
        await assert.rejects(run('down', 'second'));
        assert.ok(existsSync(join(active, '.forwarding-state')));
        assert.equal(JSON.parse(readFileSync(join(active, callKey('second') + '.json'))).rulesRemoved, true);
        const previousRules = readFileSync(log, 'utf8');
        await run('down', 'second');
        assert.equal(readFileSync(log, 'utf8'), previousRules, 'retry must not delete the already removed rules');
      } else await run('down', 'second');
      assert.deepEqual(JSON.parse(readFileSync(state)), initial);
      assert.ok(!existsSync(join(active, '.forwarding-state')));
      assert.deepEqual(readdirSync(active).filter(name => name.endsWith('.json')), []);
      await run('up', 'first');
      await run('down', 'first');
      assert.deepEqual(JSON.parse(readFileSync(state)), initial, 'new call group takes a fresh baseline');
    } finally { rmSync(root, { recursive: true, force: true }); }
  });
}
