import test from 'node:test';
import assert from 'node:assert/strict';
import { EgressPolicy } from '../src/ppp.js';
import { callKey } from '../bin/sipfax-call-key.mjs';

test('two PPP leases receive disjoint firewall and MSS rules', () => {
  const policy = new EgressPolicy({ clientCidr: '10.64.0.0/24', upstreamTcpMss: 536 });
  const descriptors = [2, 3].map(octet => policy.leaseDescriptor({
    callId: `caller${octet}`,
    lease: { localAddress: '10.64.0.1', clientAddress: `10.64.0.${octet}` }
  }));
  for (const [index, descriptor] of descriptors.entries()) {
    const address = `10.64.0.${index + 2}/32`;
    assert.equal(descriptor.clientCidr, address);
    for (const rules of [descriptor.nft.up, descriptor.iptables.up, descriptor.iptables.down]) {
      const text = rules.join('\n');
      assert.ok(text.includes(address));
      assert.ok(!text.includes('10.64.0.0/24'));
      assert.ok(!text.includes(`10.64.0.${index === 0 ? 3 : 2}/32`));
    }
    assert.ok(descriptor.nft.up.some(rule => rule.includes(address) && rule.includes('maxseg')));
  }
  assert.equal(policy.clientCidr, '10.64.0.0/24');
});

test('upstream MSS is optional and rejects unsafe or invalid limits', () => {
  const defaults = new EgressPolicy();
  assert.ok(defaults.firewallRules().every(rule => !rule.includes('TCPMSS')));
  assert.ok(defaults.firewallRulesNft().every(rule => !rule.includes('maxseg')));
  for (const value of [0, 255, 1461, -1, 536.5, NaN, '536', '536; flush ruleset']) {
    assert.throws(() => new EgressPolicy({ upstreamTcpMss: value }), /upstreamTcpMss/);
  }
});

test('MSS limits only advertisements to clients, never raises smaller values, and cleans up', () => {
  const policy = new EgressPolicy({ clientCidr: '10.64.0.0/24', upstreamTcpMss: 536 });
  const rule = policy.firewallRules().find(rule => rule.includes('TCPMSS'));
  assert.match(rule, /POSTROUTING -d 10\.64\.0\.0\/24/);
  assert.match(rule, /--tcp-flags SYN,RST SYN/);
  assert.match(rule, /--mss 537:65535 -j TCPMSS --set-mss 536$/);
  assert.ok(policy.firewallRules({ action: 'down' }).includes(rule.replace(' -A ', ' -D ')));
  const nft = policy.firewallRulesNft({ tableSuffix: 'mss_test' });
  assert.ok(nft.some(rule => rule.includes('hook postrouting priority mangle')));
  const clamp = nft.find(rule => rule.includes('maxseg'));
  assert.match(clamp, /ip daddr 10\.64\.0\.0\/24/);
  assert.match(clamp, /tcp flags & \(syn \| rst\) == syn/);
  assert.match(clamp, /maxseg size > 536 tcp option maxseg size set 536$/);
  assert.ok(policy.firewallRulesNft({ tableSuffix: 'mss_test', action: 'down' }).includes(`delete table inet sipfax_${callKey('mss_test')}`));
  assert.equal(policy.diagnostics().upstreamTcpMss, 536);
});
