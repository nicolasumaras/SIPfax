import assert from 'node:assert/strict';
import { test } from 'node:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { resolveModemCommand } from '../src/modem-command.js';
import { SipfaxConfig } from '../src/config.js';

test('native V.90 selection respects custom commands and independent binary overrides', () => {
  const env = { SIPFAX_SOFTMODEM_BINARY: '/soft', SIPFAX_SLMODEM_BRIDGE: '/smart', SIPFAX_LINMODEM_BINARY: '/native' };
  assert.equal(resolveModemCommand({ engine: 'linmodem', command: null }, {}), '/opt/sipfax/bin/sipfax-linmodem');
  assert.equal(resolveModemCommand({ engine: 'linmodem' }, env), '/native');
  assert.equal(resolveModemCommand({ engine: 'spandsp' }, env), '/soft');
  assert.equal(resolveModemCommand({ engine: 'slmodem' }, env), '/smart');
  assert.equal(resolveModemCommand({ engine: 'linmodem', command: '/capture-wrapper' }, env), '/capture-wrapper');
});

test('native engine seeds V.90 without overriding saved modem selection', () => {
  const dir = mkdtempSync(join(tmpdir(), 'sipfax-native-'));
  try {
    const path = join(dir, 'config.json');
    const { config, seeded } = SipfaxConfig.load({ path, env: { SIPFAX_MODEM_ENGINE: 'LINMODEM' } });
    assert.equal(seeded, true);
    assert.equal(config.modem.engine, 'linmodem');
    assert.equal(config.modem.modulation, 'v90');
    assert.equal(config.modem.command, null);
    config.save();
    const loaded = SipfaxConfig.load({ path, env: { SIPFAX_MODEM_ENGINE: 'spandsp', SIPFAX_MODEM_COMMAND: '/wrong' } });
    assert.equal(loaded.seeded, false);
    assert.deepEqual(loaded.config.modem, config.modem);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
