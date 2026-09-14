// Run only in a fresh disposable namespace: sudo unshare --net node tools/tests/egress-kernel.mjs
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readdirSync, readlinkSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { EgressPolicy } from '../../src/ppp.js';
import { callKey } from '../../bin/sipfax-call-key.mjs';

assert.equal(process.getuid(), 0, 'Root is required inside the disposable network namespace');
assert.notEqual(readlinkSync('/proc/self/ns/net'), readlinkSync('/proc/1/ns/net'), 'Refusing the host network namespace');
const command = (name, args, input) => execFileSync(name, args, { encoding: 'utf8', input });
assert.deepEqual(JSON.parse(command('ip', ['-j', 'link'])).map(link => link.ifname), ['lo'], 'Namespace must contain only loopback');
const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const root = mkdtempSync(join(tmpdir(), 'sipfax-kernel-'));
const leases = join(root, 'leases');
const active = join(root, 'active');
mkdirSync(leases);
const env = { ...process.env, SIPFAX_PPP_LEASE_DIR: leases, SIPFAX_PPP_ACTIVE_DIR: active };
delete env.SIPFAX_EGRESS_HELD_LOCK;
delete env.SIPFAX_SYSCTL_COMMAND;
const hook = (action, id) => execFileSync(process.execPath,
  [join(repository, 'bin/sipfax-egress-apply'), action, id, id === 'first' ? 'ppp0' : 'ppp1'], { env });
const sysctl = (key, value) => command('sysctl', value === undefined ? ['-n', key] : ['-w', `${key}=${value}`]).trim();
const snapshot = () => {
  const result = { 'net.ipv4.ip_forward': sysctl('net.ipv4.ip_forward') };
  for (const line of command('sysctl', ['-a', '--pattern', '^net.ipv4.conf\\..*\\.forwarding$']).trim().split('\n')) {
    const [key, value] = line.split(/\s*=\s*/); result[key] = value;
  }
  return result;
};
const results = [];
try {
  for (const name of ['eth0', 'eth1', 'unrelated']) command('ip', ['link', 'add', name, 'type', 'dummy']);
  command('nft', ['add', 'table', 'inet', 'unrelated']);
  for (const globalValue of ['0', '1']) {
    sysctl('net.ipv4.ip_forward', globalValue);
    sysctl('net.ipv4.conf.eth0.forwarding', '0');
    sysctl('net.ipv4.conf.eth1.forwarding', '1');
    sysctl('net.ipv4.conf.unrelated.forwarding', '0');
    const before = snapshot();
    for (const [i, callId] of ['first', 'second'].entries()) {
      const descriptor = new EgressPolicy({ operatorUrl: '', outboundInterface: `eth${i}` }).leaseDescriptor({
        callId, lease: { localAddress: '10.64.0.1', clientAddress: `10.64.0.${i + 2}` }
      });
      writeFileSync(join(leases, callKey(callId) + '.json'), JSON.stringify(descriptor));
    }
    hook('up', 'first'); hook('up', 'second');
    assert.equal(sysctl('net.ipv4.ip_forward'), '1');
    assert.equal((command('nft', ['list', 'tables']).match(/sipfax_/g) ?? []).length, 4);
    hook('down', 'first');
    assert.equal(sysctl('net.ipv4.ip_forward'), '1');
    assert.equal(sysctl('net.ipv4.conf.eth1.forwarding'), '1');
    assert.equal((command('nft', ['list', 'tables']).match(/sipfax_/g) ?? []).length, 2);
    hook('down', 'second');
    assert.deepEqual(snapshot(), before, 'Restore every original forwarding value');
    assert.equal(command('nft', ['list', 'tables']).trim(), 'table inet unrelated');
    assert.deepEqual(readdirSync(active).filter(name => name.endsWith('.json')), []);
    assert.ok(!readdirSync(active).includes('.forwarding-state'));
    results.push({ originalGlobalForwarding: globalValue, passed: true });
  }
  console.log(JSON.stringify({ kernelRoutingChecks: results, passed: true }));
} finally { rmSync(root, { recursive: true, force: true }); }
