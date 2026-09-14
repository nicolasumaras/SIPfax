#!/usr/bin/env python3
"""Migrate PPP credentials without giving sipfax write access to /etc/ppp."""
import os
from pathlib import Path
import pwd
import tempfile


def prepare(root, uid, gid):
    root = Path(root)
    managed = root / 'var/lib/sipfax/ppp-secrets'
    managed.mkdir(parents=True, exist_ok=True)
    os.chown(managed, uid, gid)
    managed.chmod(0o700)
    etc = root / 'etc/ppp'
    etc.mkdir(parents=True, exist_ok=True)
    for name in ('chap-secrets', 'pap-secrets'):
        source = etc / name
        target = managed / name
        if source.is_symlink():
            if source.resolve() != target.resolve() or not target.is_file():
                raise RuntimeError('Unexpected or broken credential link: ' + str(source))
            os.chown(target, uid, gid)
            target.chmod(0o600)
            continue
        data = source.read_bytes() if source.exists() else b''
        if target.exists() and target.read_bytes() != data:
            raise RuntimeError('Conflicting credential copy: ' + str(target))
        if source.exists():
            backup = etc / (name + '.pre-sipfax')
            # Never replace a previous rollback copy.
            if not backup.exists():
                fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(data)
        fd, pending = tempfile.mkstemp(prefix='.credentials-', dir=managed)
        try:
            with os.fdopen(fd, 'wb') as stream:
                os.fchown(stream.fileno(), uid, gid)
                stream.write(data)
            os.replace(pending, target)
        finally:
            if os.path.exists(pending):
                os.unlink(pending)
        with tempfile.TemporaryDirectory(prefix='.sipfax-link-', dir=etc) as temp:
            link = Path(temp) / name
            link.symlink_to(target)
            os.replace(link, source)


if __name__ == '__main__':
    if os.geteuid() != 0:
        raise SystemExit('Run as root after stopping SIPfax and its PPP sessions')
    user = pwd.getpwnam('sipfax')
    prepare('/', user.pw_uid, user.pw_gid)
    print('PPP credential links prepared; existing rollback copies preserved')
