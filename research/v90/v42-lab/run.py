#!/usr/bin/env python3
"""Qualify a pinned V.42 reference in isolation; never install or contact modems."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PATCH_FROM = 'LAPM_S_RNR  :  LAPM_S_RR, 1);\n}'
PATCH_TO = 'LAPM_S_RNR  :  LAPM_S_RR, frame[2] & 1);\n}'
MODULES = ['v42', 'hdlc', 'crc', 'bit_operations', 'logging', 'alloc', 'async']

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='Pristine pinned SpanDSP checkout/tarball root')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    source = args.source.resolve() / 'src'
    manifest = json.loads((HERE / 'source-hashes.json').read_text())
    for name, expected in manifest['files'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            ap.error('Pinned source differs: ' + name)
    original = (source / 'v42.c').read_text()
    # The first occurrence is the common I/RR command response helper.
    patched = original.replace(PATCH_FROM, PATCH_TO, 1)
    if patched == original:
        ap.error('Expected response helper was not found')
    results = []
    with tempfile.TemporaryDirectory(prefix='sipfax-v42-lab-') as td:
        build = Path(td)
        for variant, content, defines in [
            ('upstream', original, []),
            ('corrected', patched, []),
            ('variable', patched, ['-DVARIABLE_FRAMES']),
            ('recovery', patched, ['-DERROR_STOP_SECONDS=30']),
        ]:
            implementation = build / (variant + '.c')
            implementation.write_text(content)
            binary = build / variant
            command = ['gcc', '-O2', '-Wall', '-DHAVE_STDBOOL_H', '-DHAVE_MALLOC_H',
                       '-DHAVE_POSIX_MEMALIGN', '-D_GNU_SOURCE', '-I'+str(source), *defines,
                       str(HERE/'check.c'), str(implementation),
                       *[str(source/(n+'.c')) for n in MODULES if n != 'v42'],
                       '-o', str(binary)]
            subprocess.run(command, check=True)
            if variant == 'upstream':
                cases = [(0, 0, 1, 0, 0, 0), (0, 1, 1, 0, 0, 0)]
            elif variant == 'recovery':
                cases = [(11000, 1, 1, 80, 1, 0), (11000, 1, 1, 120, 1, 0)]
            else:
                cases = [(period, 1, 1, delay, 1, 0)
                         for delay in (0, 40, 80, 120)
                         for period in (0, 11000, 19001, 23003, 50021)]
                cases += [(0, 1, 1, 80, 0, 160), (0, 1, 0, 80, 1, 0)]
            for case in cases:
                run = subprocess.run([str(binary), *map(str, case)], capture_output=True,
                                     text=True, timeout=30)
                row = {'variant': variant, 'parameters': dict(zip(
                    ['bit_error_period', 'asymmetric', 'detect', 'delay_ms', 'busy', 'burst_bits'], case)),
                    'exit_code': run.returncode,
                    'results': [json.loads(line) for line in run.stdout.splitlines()],
                    'diagnostics': run.stderr.splitlines()}
                results.append(row)
                print(variant, case, 'PASS' if run.returncode == 0 else 'INCOMPLETE', flush=True)
    report = {'source_revision': manifest['revision'],
              'patch': 'Echo the incoming poll bit in RR/RNR response final bit (V.42 8.4.2).',
              'scope': 'Two reference peers, 65536 exact bytes each direction; no hardware or full conformance claim.',
              'cases': results}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    # Report actual failures instead of redefining them as passing regressions.
    failures = [r for r in results if r['variant'] != 'upstream' and r['exit_code']]
    if failures:
        print(f'{len(failures)} corrected cases remain incomplete; see report.')
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
