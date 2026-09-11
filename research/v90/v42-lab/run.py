#!/usr/bin/env python3
"""Qualify a pinned V.42 reference in isolation; never install or contact modems."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from negotiation_patch import apply as patch_negotiation

HERE = Path(__file__).resolve().parent
PATCH_FROM = 'LAPM_S_RNR  :  LAPM_S_RR, 1);\n}'
PATCH_TO = 'LAPM_S_RNR  :  LAPM_S_RR, frame[2] & 1);\n}'
MODULES = ['v42', 'hdlc', 'crc', 'bit_operations', 'logging', 'alloc', 'async']

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='Pristine pinned SpanDSP checkout/tarball root')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sanitizers', action='store_true', help='Build with ASan/UBSan; requires installed runtimes')
    args = ap.parse_args()
    source = args.source.resolve() / 'src'
    manifest = json.loads((HERE / 'source-hashes.json').read_text())
    for name, expected in manifest['files'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            ap.error('Pinned source differs: ' + name)
    original = (source / 'v42.c').read_text()
    # The first occurrence is the common I/RR command response helper.
    final_bit_only = original.replace(PATCH_FROM, PATCH_TO, 1)
    timer_from = 'if (ss->bit_timer == 0)\n            t401_start(ss);'
    timer_to = 'if (ss->bit_timer == 0 || ss->bit_timer_func != t401_expired)\n            t401_start(ss);'
    if final_bit_only.count(timer_from) != 1:
        ap.error('Expected I-frame acknowledgement timer check was not found')
    patched = final_bit_only.replace(timer_from, timer_to, 1)
    shift_from = 's->neg.rxstream = (s->neg.rxstream << 1) | new_bit;'
    shift_to = 's->neg.rxstream = ((s->neg.rxstream & 0x1FF) << 1) | new_bit;'
    if patched.count(shift_from) != 1:
        ap.error('Expected detection shift register was not found')
    patched = patched.replace(shift_from, shift_to, 1)
    if final_bit_only == original:
        ap.error('Expected response helper was not found')
    pre_framing = patched
    insertion = 'SPAN_DECLARE(void) lapm_receive(void *user_data, const uint8_t *frame, int len, int ok)'
    if patched.count(insertion) != 1:
        ap.error('Expected LAPM receive entry point was not found')
    patched = patched.replace(insertion, (HERE/'framing_guard.c').read_text()+'\n'+insertion, 1)
    receive_guard = '    if (!ok)\n        return;\n\n    switch ((frame[1] & LAPM_FRAMETYPE_MASK))'
    replacement = '    if (!ok || !frame || len < 2)\n        return;\n    if ((frame[1] & LAPM_FRAMETYPE_MASK) != LAPM_FRAMETYPE_U && len < 3)\n        return;\n    if ((frame[1] & LAPM_FRAMETYPE_MASK) == LAPM_FRAMETYPE_U\n        && (frame[1] & 0xEC) == LAPM_U_XID && !validated_xid(frame, len))\n        return;\n\n    switch ((frame[1] & LAPM_FRAMETYPE_MASK))'
    if patched.count(receive_guard) != 1:
        ap.error('Expected receive dispatch guard was not found')
    patched = patched.replace(receive_guard, replacement, 1)
    for old in ['put_net_unaligned_uint32(buf, 0x8A890000);  /* Bits 2, 4, 8 , 9, 12, and 16 set */']:
        if patched.count(old) != 1:
            ap.error('Expected XID serializer field was not found')
        patched = patched.replace(old, old+'\n    buf += 4;', 1)
    pre_negotiation = patched
    patched = patch_negotiation(patched)
    results = []
    with tempfile.TemporaryDirectory(prefix='sipfax-v42-lab-') as td:
        build = Path(td)
        for variant, content, defines in [
            ('upstream', original, []),
            ('final-bit-only', final_bit_only, []),
            ('pre-framing', pre_framing, []),
            ('pre-negotiation', pre_negotiation, []),
            ('corrected', patched, []),
            ('outage-short', patched, ['-DOUTAGE_SECONDS=2']),
            ('outage-long', patched, ['-DOUTAGE_SECONDS=30', '-DOUTAGE_DISCONNECT']),
            ('negotiated-limits', patched, ['-DNEGOTIATED_LIMITS']),
            ('variable', patched, ['-DVARIABLE_FRAMES']),
            ('recovery', patched, ['-DERROR_STOP_SECONDS=30']),
            ('long-stress', patched, ['-DTEST_SECONDS=600', '-DERROR_STOP_SECONDS=600']),
        ]:
            implementation = build / (variant + '.c')
            implementation.write_text(content)
            binary = build / variant
            command = ['gcc', '-O2', '-Wall', '-DHAVE_STDBOOL_H', '-DHAVE_MALLOC_H',
                       '-DHAVE_POSIX_MEMALIGN', '-D_GNU_SOURCE', '-I'+str(source), *defines,
                       *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitizers else []),
                       str(HERE/'check.c'), str(implementation),
                       *[str(source/(n+'.c')) for n in MODULES if n != 'v42'],
                       '-o', str(binary)]
            subprocess.run(command, check=True)
            if variant in ('pre-negotiation', 'corrected'):
                negotiation_binary = build / (variant+'-negotiation')
                negotiation_command = [str(HERE/'negotiation.c') if arg == str(HERE/'check.c') else
                                       str(negotiation_binary) if arg == str(binary) else arg for arg in command]
                subprocess.run(negotiation_command, check=True)
                trial = subprocess.run([str(negotiation_binary)], capture_output=True, text=True, timeout=30)
                results.append({'variant': variant+'-negotiation', 'parameters': {},
                                'exit_code': trial.returncode,
                                'results': [json.loads(line) for line in trial.stdout.splitlines()],
                                'diagnostics': trial.stderr.splitlines()})
                if variant == 'pre-negotiation':
                    continue
            if variant in ('pre-framing', 'corrected'):
                frame_binary = build / (variant+'-frames')
                frame_command = [str(HERE/'frames.c') if arg == str(HERE/'check.c') else
                                 str(frame_binary) if arg == str(binary) else arg for arg in command]
                subprocess.run(frame_command, check=True)
                for mode in ('wire', 'invalid'):
                    frame_run = subprocess.run([str(frame_binary), mode], capture_output=True, text=True, timeout=30)
                    results.append({'variant': variant+'-frames', 'parameters': {'mode': mode},
                                    'exit_code': frame_run.returncode,
                                    'results': [json.loads(line) for line in frame_run.stdout.splitlines()],
                                    'diagnostics': frame_run.stderr.splitlines()})
                if variant == 'pre-framing':
                    continue
            if variant == 'upstream':
                cases = [(0, 0, 1, 0, 0, 0), (0, 1, 1, 0, 0, 0)]
            elif variant == 'final-bit-only':
                cases = [(11000, 1, 1, delay, 1, 0) for delay in (80, 120)]
            elif variant in ('outage-short', 'outage-long'):
                cases = [(0, 1, 1, delay, 0, 0) for delay in (0, 80)]
            elif variant == 'negotiated-limits':
                cases = [(0, 1, 1, 0, 0, 0), (0, 1, 1, 80, 1, 0),
                         (50021, 1, 1, 40, 1, 0), (0, 1, 0, 40, 0, 0)]
            elif variant == 'long-stress':
                cases = [(11000, 1, 1, 120, 1, 0)]
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
              'patch': 'Echo P in response F (8.4.2); replace T403 with T401 when sending an I frame (8.4.1); bound the detection shift register to ten bits; validate complete XID envelopes before dispatch; advance past serialized HDLC options.',
              'scope': 'Two reference peers, 65536 exact bytes each direction; primary deadline 120 simulated seconds; long-stress diagnostic retains continuous errors up to 600 seconds; no hardware or full conformance claim.',
              'negotiation_patch': 'Map peer TX to local RX; negotiate relative to standard defaults; reply with selected values; preserve parameters through link establishment; restore preferences on modem restart.',
              'sanitizers': args.sanitizers,
              'cases': results}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    # Report actual failures instead of redefining them as passing regressions.
    failures = [r for r in results if r['variant'] not in ('upstream', 'final-bit-only', 'pre-framing-frames', 'pre-negotiation-negotiation') and r['exit_code']]
    if failures:
        print(f'{len(failures)} corrected cases remain incomplete; see report.')
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
