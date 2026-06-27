# vendor/slmodem — SmartLink soft-modem datapump (vendored)

This directory stages the **SmartLink `slmodemd` daemon + `dsplibs.o` datapump** that the
higher-speed modem engine (V.32bis / V.34) drives. It is fetched, not committed wholesale, by
`fetch.sh`. Build/run is **32-bit x86 Linux only** (the datapump is an i386 ELF object).

## What this is and the licensing split (important)

- **`slmodemd` wrapper + AT/control source** (`modem*.c`, `modem*.h`): **GPL-2.0** (SmartLink,
  via Debian's `sl-modem-daemon`, as patched by D-Modem to use a socket audio transport).
- **`dsplibs.o`**: the actual V.21/V.22/V.32/V.32bis/V.34/V.90/V.92 datapump. **Closed-source but
  gratis** — free to download and redistribute, **no payment**. ("non-free" in Debian = not
  open-licensed, *not* "costs money".) It is a binary blob; we cannot modify or rebuild it.
- Constraint: **i386 only** → the engine process runs 32-bit (multilib).

This is the same approach **D-Modem** (`AonCyberLabs/D-Modem`, GPL-2.0) uses. We reuse D-Modem's
already-patched `slmodemd/` (kernel driver replaced by an `AF_UNIX` socket transport) because our
bridge plugs into that same socket. See `../slmodem-bridge/DESIGN.md`.

## Provenance / pinned checksums

| Item | Value |
| --- | --- |
| slmodem version | 2.9.11 |
| Source repo | `https://github.com/AonCyberLabs/D-Modem` (subdir `slmodemd/`) |
| Pinned commit | `636959b37b592b87a47c6da2069149961cd70ccf` |
| `dsplibs.o` SHA-256 | `1f3e56d0dfae1a6aaf4eb6fcc4875a4524905e010d5758114cde288b3cf0b379` |
| `dsplibs.o` type | ELF 32-bit LSB relocatable, Intel 80386, not stripped |

`fetch.sh` verifies the `dsplibs.o` SHA-256 against the value above and aborts on mismatch, so
builds are reproducible and the blob's identity is pinned.

> Upstream alternative for the GPL source (not the blob): `leggewie/pkg-sl-modem` /
> Debian `sl-modem-source`. The blob is identical across these distributions of 2.9.11.

## Build prerequisites (Debian 12, on the test VM)

```sh
sudo apt-get install -y gcc-multilib libc6-dev-i386 make
```

## Usage

```sh
./fetch.sh                 # clone pinned D-Modem, stage slmodemd/ + dsplibs.o here, verify sha256
make -C slmodemd           # builds the 32-bit slmodemd (links dsplibs.o)
```

`slmodemd` is then launched by the SIPfax engine as `slmodemd -e <sipfax-slmodem-bridge>`.

## Why not commit the blob directly?

It is gratis and redistributable, so we *may* — but pinning by checksum + fetch keeps the repo
small and makes the blob's provenance explicit and auditable. If offline/reproducible builds need
it in-tree, drop the verified `dsplibs.o` here and point the build at it; the checksum above is
the gate either way.
