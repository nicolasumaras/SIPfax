#!/usr/bin/env bash
#
# Stage the SmartLink slmodemd source + dsplibs.o datapump blob for the SIPfax
# higher-speed modem engine. Pins to a known commit and verifies the blob's
# SHA-256 so builds are reproducible. See README.md for provenance/licensing.
#
# Build/run target is 32-bit x86 Linux (multilib). This script only stages files;
# it does not build.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- pinned provenance (keep in sync with README.md) -------------------------
REPO_URL="https://github.com/AonCyberLabs/D-Modem"
PIN_COMMIT="636959b37b592b87a47c6da2069149961cd70ccf"
DSPLIBS_SHA256="1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[fetch] cloning $REPO_URL @ $PIN_COMMIT ..."
git clone --quiet "$REPO_URL" "$WORK/dmodem"
git -C "$WORK/dmodem" checkout --quiet "$PIN_COMMIT"

echo "[fetch] verifying dsplibs.o checksum ..."
got="$(sha256sum "$WORK/dmodem/slmodemd/dsplibs.o" | awk '{print $1}')"
if [ "$got" != "$DSPLIBS_SHA256" ]; then
  echo "[fetch] ERROR: dsplibs.o sha256 mismatch" >&2
  echo "  expected $DSPLIBS_SHA256" >&2
  echo "  got      $got" >&2
  exit 1
fi
echo "[fetch] dsplibs.o OK ($got)"

echo "[fetch] staging slmodemd/ into $HERE ..."
rm -rf "$HERE/slmodemd"
cp -a "$WORK/dmodem/slmodemd" "$HERE/slmodemd"

echo "[fetch] applying SIPfax slmodemd patch ..."
# Replaces the fork-based -e audio transport with a direct AF_UNIX connect to the
# bridge, and lets SIPFAX_TTY_LINK override the /dev/ttySLn link name (so two
# instances can coexist). See 0001-sipfax-socket-transport-and-ttylink.patch.
patch -p1 -d "$HERE" < "$HERE/0001-sipfax-socket-transport-and-ttylink.patch"

echo "[fetch] done. Next: 'sudo apt-get install -y gcc-multilib libc6-dev-i386 make' then 'make -C $HERE/slmodemd'"
