#!/usr/bin/env python3
"""Cross-check one DialUpLab POST against the receiving server's audit record."""
import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


def audit(probe, records, attempt, size, peer='10.64.0.2'):
    if not attempt or type(size) is not int or not 1 <= size <= 1048576:
        raise ValueError('A nonempty attempt and size 1..1048576 are required')
    payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
    digest = hashlib.sha256(payload).hexdigest()
    errors = []
    expected = {'method': 'POST', 'sourceIpv4': peer, 'uploadComplete': True,
                'uploadBytesWritten': size, 'uploadSha256': digest,
                'complete': True, 'httpStatus': 200, 'bodyBytes': 64,
                'bodySha256': hashlib.sha256(digest.encode('ascii')).hexdigest()}
    for key, value in expected.items():
        if type(probe.get(key)) is not type(value) or probe.get(key) != value:
            errors.append('client ' + key + ' mismatch or missing')
    if probe.get('error') or probe.get('limitExceeded'):
        errors.append('client reported an error or response limit')
    for position in ['statisticsBefore', 'statisticsAfter']:
        stats = probe.get(position) or {}
        for counter in ['crcErrors', 'timeoutErrors', 'alignmentErrors', 'hardwareOverruns', 'framingErrors', 'bufferOverruns']:
            if type(stats.get(counter)) is not int or stats[counter] != 0:
                errors.append(position + ': ' + counter + ' nonzero or missing')
    before = (probe.get('statisticsBefore') or {}).get('durationMilliseconds')
    after = (probe.get('statisticsAfter') or {}).get('durationMilliseconds')
    if type(before) is not int or type(after) is not int or not 0 <= before <= after:
        errors.append('RAS duration missing or reset')
    matching = [r for r in records if urlsplit(r.get('path', '')).path == '/upload'
                and parse_qs(urlsplit(r.get('path', '')).query).get('attempt') == [attempt]]
    if len(matching) != 1:
        errors.append('expected exactly one server record for this attempt')
    else:
        record = matching[0]
        for key, value in {'peer': peer, 'status': 200, 'bytesReceived': size, 'patternValid': True, 'sha256': digest}.items():
            if type(record.get(key)) is not type(value) or record.get(key) != value:
                errors.append('server ' + key + ' mismatch or missing')
        if record.get('error'):
            errors.append('server reported an error')
    return {'attempt': attempt, 'requestedBytes': size, 'expectedSha256': digest,
            'payloadVerified': not errors, 'errors': errors,
            'scope': 'Client/server payload integrity only. PPP packet capture, same-call survival and complete teardown must be verified separately.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--server-audit', required=True, type=Path)
    parser.add_argument('--attempt', required=True)
    parser.add_argument('--bytes', required=True, type=int)
    parser.add_argument('--peer', default='10.64.0.2')
    args = parser.parse_args()
    result = audit(json.loads(args.probe.read_text()),
                   [json.loads(line) for line in args.server_audit.read_text().splitlines() if line.strip()],
                   args.attempt, args.bytes, args.peer)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['payloadVerified'] else 1)
