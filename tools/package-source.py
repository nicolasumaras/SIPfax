#!/usr/bin/env python3
"""Package a committed revision, never the working tree, with a checksum manifest."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def package(repo, revision, output):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args])

    commit = git('rev-parse', '--verify', '--end-of-options', revision + '^{commit}').decode().strip()
    tree = git('rev-parse', commit + '^{tree}').decode().strip()
    prefix = 'sipfax-' + commit + '/'
    archive = git('-c', 'tar.umask=0022', 'archive', '--format=tar', '--prefix=' + prefix, commit)
    output.mkdir(parents=True, exist_ok=True)
    name = prefix[:-1] + '.tar.gz'
    # An empty gzip filename and zero timestamp remove output-path/time variation.
    with tempfile.TemporaryFile() as compressed:
        with gzip.GzipFile(filename='', mode='wb', fileobj=compressed, mtime=0, compresslevel=9) as writer:
            writer.write(archive)
        compressed.seek(0)
        payload = compressed.read()
    manifest = {
        'formatVersion': 1,
        'kind': 'source',
        'commit': commit,
        'tree': tree,
        'archive': name,
        'archiveSha256': hashlib.sha256(payload).hexdigest(),
        'archiveBytes': len(payload),
        'archivePrefix': prefix,
        'nativeBuildRequired': True,
        'buildInstruction': 'Build vendor/linmodem on the deployment host; run deploy/install-systemd.sh --engine=linmodem --check there.',
    }
    for filename, content in [(name, payload), (prefix[:-1] + '.json', (json.dumps(manifest, indent=2) + '\n').encode())]:
        target = output / filename
        if target.exists() and target.read_bytes() != content:
            raise RuntimeError('Refusing to overwrite a different artifact: ' + str(target))
        target.write_bytes(content)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--revision', required=True, help='Explicit committed revision to package')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(package(args.repo, args.revision, args.output), indent=2))
