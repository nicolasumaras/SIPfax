"""Source release identity survives checkout paths, dirty files, and output names."""
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('package_source', Path(__file__).resolve().parents[1] / 'package-source.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SourcePackageTest(unittest.TestCase):
    def test_committed_identity_and_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / 'first'
            def git(*args):
                return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.DEVNULL)
            repo.mkdir()
            git('init')
            git('config', 'user.name', 'Release test')
            git('config', 'user.email', 'release@example.invalid')
            (repo / 'source.c').write_text('committed\n')
            (repo / 'launcher').write_text('#!/bin/sh\nexit 0\n')
            (repo / 'launcher').chmod(0o755)
            git('add', '.')
            git('commit', '-m', 'fixture')
            commit = git('rev-parse', 'HEAD').decode().strip()
            subprocess.run(['git', 'clone', '--quiet', str(repo), str(root / 'second')], check=True)
            (repo / 'source.c').write_text('uncommitted\n')
            (repo / 'credentials.env').write_text('must not ship\n')
            one = module.package(repo, commit, root / 'out-a')
            two = module.package(root / 'second', commit, root / 'out-b')
            self.assertEqual(one, two)
            a = (root / 'out-a' / one['archive']).read_bytes()
            self.assertEqual(a, (root / 'out-b' / two['archive']).read_bytes())
            self.assertEqual(hashlib.sha256(a).hexdigest(), one['archiveSha256'])
            with tarfile.open(fileobj=io.BytesIO(a), mode='r:gz') as tar:
                prefix = one['archivePrefix']
                self.assertEqual(tar.extractfile(prefix + 'source.c').read(), b'committed\n')
                self.assertEqual(tar.getmember(prefix + 'launcher').mode, 0o755)
                self.assertFalse(any(p.endswith('credentials.env') for p in tar.getnames()))
            with self.assertRaises(subprocess.CalledProcessError):
                module.package(repo, 'does-not-exist', root / 'bad')
            (root / 'out-a' / one['archive']).write_bytes(b'wrong')
            with self.assertRaises(RuntimeError):
                module.package(repo, commit, root / 'out-a')


if __name__ == '__main__':
    unittest.main()
