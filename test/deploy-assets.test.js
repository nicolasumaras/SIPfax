import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

test('systemd unit keeps Node 24 compatible executable memory policy', async () => {
  const unit = await readFile(new URL('../deploy/sipfax.service', import.meta.url), 'utf8');

  assert.match(unit, /MemoryDenyWriteExecute=false/);
  assert.doesNotMatch(unit, /MemoryDenyWriteExecute=true/);
});

test('systemd unit allows SIPfax runtime artifact directories', async () => {
  const [unit, installer, runbook] = await Promise.all([
    readFile(new URL('../deploy/sipfax.service', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/install-systemd.sh', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/README.md', import.meta.url), 'utf8')
  ]);

  assert.match(unit, /ProtectSystem=strict/);
  assert.match(unit, /ReadWritePaths=.*\/var\/cache\/sipfax.*\/var\/log\/sipfax/);
  assert.match(installer, /install -d -m 0755 -o sipfax -g sipfax \/var\/cache\/sipfax/);
  assert.match(installer, /install -d -m 0755 -o sipfax -g sipfax \/var\/log\/sipfax/);
  assert.match(runbook, /\/var\/log\/sipfax/);
});

test('deploy runbook documents the Node 24 systemd hardening exception', async () => {
  const runbook = await readFile(new URL('../deploy/README.md', import.meta.url), 'utf8');

  assert.match(runbook, /MemoryDenyWriteExecute=false/);
  assert.match(runbook, /Node `24\.x`\/V8/);
  assert.match(runbook, /fresh Debian 12 deployments start cleanly/);
});

test('deployment assets default to the spandsp soft-modem worker', async () => {
  const [envExample, installer, runbook] = await Promise.all([
    readFile(new URL('../deploy/sipfax.env.example', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/install-systemd.sh', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/README.md', import.meta.url), 'utf8')
  ]);

  assert.match(envExample, /# SIPFAX_SOFTMODEM_BINARY=\/opt\/sipfax\/bin\/sipfax-softmodem/);
  assert.match(envExample, /# SIPFAX_MODEM_COMMAND=\/usr\/local\/bin\/sipfax-modem-bridge/);
  assert.match(envExample, /SIPFAX_MODEM_ARGS=/);
  assert.match(installer, /vendor\/sipfax-softmodem\/sipfax-softmodem/);
  assert.match(installer, /\/opt\/sipfax\/bin\/sipfax-softmodem/);
  assert.match(installer, /\/etc\/ppp\/ip-up\.d\/sipfax/);
  assert.match(installer, /\/etc\/ppp\/ip-down\.d\/sipfax/);
  assert.match(runbook, /spandsp soft-modem worker/);
  assert.match(runbook, /libspandsp2/);
  assert.match(runbook, /\/opt\/sipfax\/bin\/sipfax-softmodem/);
  assert.match(runbook, /media\.modem\.type/);
  assert.match(runbook, /media\.modem\.modulation/);
  assert.doesNotMatch(runbook, /default live call path uses SIPfax's in-process/);
});

test('deploy runbook documents pppd hooks, nftables checks, and per-call ppp secrets', async () => {
  const [envExample, runbook] = await Promise.all([
    readFile(new URL('../deploy/sipfax.env.example', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/README.md', import.meta.url), 'utf8')
  ]);

  assert.match(envExample, /per-call[\s\S]*chap-secrets\/pap-secrets/);
  assert.match(runbook, /\/etc\/ppp\/ip-up\.d\/sipfax/);
  assert.match(runbook, /\/etc\/ppp\/ip-down\.d\/sipfax/);
  assert.match(runbook, /Operator review is required/);
  assert.match(runbook, /chap-secrets/);
  assert.match(runbook, /0600/);
  assert.match(runbook, /pppd --version/);
  assert.match(runbook, /nft list ruleset \| head/);
  assert.match(runbook, /jq '\.media\.modem'/);
});
