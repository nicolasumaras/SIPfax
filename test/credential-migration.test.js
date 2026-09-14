import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';

test('credential migration preserves bytes, backups and restrictive permissions across reruns', () => {
  const script = new URL('../deploy/prepare-ppp-secrets.py', import.meta.url).pathname;
  execFileSync('python3', ['-c', `
import importlib.util, os, pathlib, tempfile
spec = importlib.util.spec_from_file_location('migration', ${JSON.stringify(script)})
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as temp:
 root = pathlib.Path(temp)
 etc = root / 'etc/ppp'
 etc.mkdir(parents=True)
 data = b'"user" * "secret" *\\n'
 (etc / 'chap-secrets').write_bytes(data)
 module.prepare(root, os.getuid(), os.getgid())
 module.prepare(root, os.getuid(), os.getgid())
 source = etc / 'chap-secrets'
 assert source.is_symlink() and source.read_bytes() == data
 assert (etc / 'chap-secrets.pre-sipfax').read_bytes() == data
 assert source.stat().st_mode & 0o777 == 0o600
 assert source.resolve().parent.stat().st_mode & 0o777 == 0o700
 source.write_bytes(b'updated')
 module.prepare(root, os.getuid(), os.getgid())
 assert source.read_bytes() == b'updated'
 assert (etc / 'chap-secrets.pre-sipfax').read_bytes() == data
 assert (etc / 'pap-secrets').read_bytes() == b''
`]);
  assert.ok(true);
});
