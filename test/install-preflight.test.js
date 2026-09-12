import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';
import { test } from 'node:test';

test('native installer preflight needs only the native worker and rejects incomplete releases', () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-install-check-'));
  const write = (path, content = '') => {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), content, { mode: 0o755 });
  };
  write('deploy/install-systemd.sh');
  copyFileSync(new URL('../deploy/install-systemd.sh', import.meta.url), join(root, 'deploy/install-systemd.sh'));
  const run = (...args) => spawnSync('bash', [join(root, 'deploy/install-systemd.sh'), ...args], { encoding: 'utf8' });
  try {
    const missingWorker = run('--engine=linmodem', '--check');
    assert.equal(missingWorker.status, 1);
    assert.match(missingWorker.stderr, /Missing executable vendor\/linmodem\/lm/);
    write('vendor/linmodem/lm', '#!/bin/sh\nexit 0\n');
    assert.match(run('--engine=linmodem', '--check').stderr, /Missing package.json/);
    for (const asset of ['package.json', 'package-lock.json', 'public/admin.html', 'src/index.js', 'bin/sipfax-call-key.mjs', 'bin/sipfax-egress-apply', 'bin/sipfax-linmodem']) write(asset);
    const native = run('--engine=linmodem', '--check');
    assert.equal(native.status, 0, native.stderr);
    assert.match(native.stdout, /preflight passed: linmodem/);
    // The execute bit alone must not let an invalid ELF worker pass preflight.
    write('vendor/linmodem/lm', '\x7fELFinvalid');
    const invalidElf = run('--engine=linmodem', '--check');
    assert.equal(invalidElf.status, 1);
    assert.match(invalidElf.stderr, /Cannot load vendor\/linmodem\/lm/);
    copyFileSync(process.execPath, join(root, 'vendor/linmodem/lm'));
    const loadableElf = run('--engine=linmodem', '--check');
    assert.equal(loadableElf.status, 0, loadableElf.stderr);
    assert.equal(run('--engine=spandsp', '--check').status, 1);
    assert.equal(run('--engine=unknown', '--check').status, 64);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
