import copy
import hashlib
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('upload_audit', Path(__file__).resolve().parents[1] / 'audit-ppp-upload.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class UploadAuditTest(unittest.TestCase):
    def setUp(self):
        self.size = 12345
        # Independent payload construction, including the final partial block.
        digest = hashlib.sha256(bytes(i % 256 for i in range(self.size))).hexdigest()
        stats = {k: 0 for k in ['crcErrors', 'timeoutErrors', 'alignmentErrors', 'hardwareOverruns', 'framingErrors', 'bufferOverruns']}
        self.probe = {'method': 'POST', 'sourceIpv4': '10.64.0.2', 'uploadComplete': True,
                      'uploadBytesWritten': self.size, 'uploadSha256': digest, 'complete': True,
                      'httpStatus': 200, 'bodyBytes': 64, 'bodySha256': hashlib.sha256(digest.encode()).hexdigest(),
                      'statisticsBefore': {**stats, 'durationMilliseconds': 1000},
                      'statisticsAfter': {**stats, 'durationMilliseconds': 8000}}
        self.server = {'path': '/upload?attempt=unique', 'peer': '10.64.0.2', 'status': 200,
                       'bytesReceived': self.size, 'patternValid': True, 'sha256': digest}

    def check(self, probe=None, records=None):
        return m.audit(self.probe if probe is None else probe, [self.server] if records is None else records, 'unique', self.size)['payloadVerified']

    def test_exact_transfer(self):
        self.assertTrue(self.check())

    def test_false_client_success(self):
        for key, bad in [('method', 'GET'), ('uploadBytesWritten', self.size-1), ('uploadSha256', 'wrong'),
                         ('sourceIpv4', '192.168.1.217'), ('bodySha256', 'wrong'), ('complete', False),
                         ('uploadComplete', 1), ('statisticsAfter', None)]:
            with self.subTest(key=key):
                probe=copy.deepcopy(self.probe);probe[key]=bad
                self.assertFalse(self.check(probe=probe))
        probe=copy.deepcopy(self.probe);probe['statisticsAfter']['crcErrors']=1
        self.assertFalse(self.check(probe=probe))
        probe=copy.deepcopy(self.probe);probe['statisticsAfter']['durationMilliseconds']=999
        self.assertFalse(self.check(probe=probe))

    def test_receipt_required_and_unambiguous(self):
        for records in [[], [self.server,self.server], [{**self.server,'path':'/upload?attempt=old'}],
                        [{**self.server,'peer':'192.168.1.217'}], [{**self.server,'bytesReceived':2}],
                        [{**self.server,'patternValid':False}], [{**self.server,'sha256':'wrong'}]]:
            self.assertFalse(self.check(records=records))


if __name__ == '__main__':
    unittest.main()
