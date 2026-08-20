"""Safe in-place source patching for the linmodem work.

Use this instead of `open(path,'w').write(...)`. A plain truncating write destroys the
file if the encode fails midway: during this work a single em-dash in a C comment raised
UnicodeEncodeError *after* v34.c had already been truncated, leaving it 0 bytes. Only the
git commit saved it.

Rules that follow from that:
  - encode to bytes BEFORE touching the target file
  - write a temp file, then os.replace() (atomic on POSIX)
  - keep patch text ASCII-only; C sources here are latin-1
"""
import os


def read(path, enc="latin-1"):
    with open(path, encoding=enc) as f:
        return f.read()


def write(path, text, enc="latin-1"):
    """Atomic write. Encodes first, so an encoding error cannot truncate the target."""
    try:
        data = text.encode(enc)
    except UnicodeEncodeError as e:
        raise SystemExit("patch aborted, target untouched: non-%s character (%s).\n"
                         "Keep patch text ASCII-only." % (enc, e))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def sub(text, old, new, what="block", count=1):
    """Replace `old` once, asserting it was actually present."""
    if old not in text:
        raise SystemExit("patch aborted: %s not found" % what)
    return text.replace(old, new, count)
