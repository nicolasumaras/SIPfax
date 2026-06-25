import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

test('systemd unit keeps Node 24 compatible executable memory policy', async () => {
  const unit = await readFile(new URL('../deploy/sipfax.service', import.meta.url), 'utf8');

  assert.match(unit, /MemoryDenyWriteExecute=false/);
  assert.doesNotMatch(unit, /MemoryDenyWriteExecute=true/);
});

test('deploy runbook documents the Node 24 systemd hardening exception', async () => {
  const runbook = await readFile(new URL('../deploy/README.md', import.meta.url), 'utf8');

  assert.match(runbook, /MemoryDenyWriteExecute=false/);
  assert.match(runbook, /Node `24\.x`\/V8/);
  assert.match(runbook, /fresh Debian 12 deployments start cleanly/);
});

test('deployment assets require an external modem backend', async () => {
  const [envExample, runbook] = await Promise.all([
    readFile(new URL('../deploy/sipfax.env.example', import.meta.url), 'utf8'),
    readFile(new URL('../deploy/README.md', import.meta.url), 'utf8')
  ]);

  assert.match(envExample, /SIPFAX_MODEM_COMMAND=\/usr\/local\/bin\/sipfax-modem-bridge/);
  assert.match(envExample, /SIPFAX_MODEM_ARGS=/);
  assert.match(runbook, /length-prefixed G\.711 frames/);
  assert.match(runbook, /fail fast at service\s+startup/);
});
