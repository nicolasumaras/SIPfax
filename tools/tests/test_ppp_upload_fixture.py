import hashlib
import importlib.util
import io
import http.client
import json
import tempfile
import threading
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('fixture', Path(__file__).parents[1]/'ppp-upload-fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

class UploadBodyTests(unittest.TestCase):
    def test_full_body_and_partial_final_chunk(self):
        for count in (1, 12345, fixture.MAX_UPLOAD):
            body = bytes(i & 255 for i in range(count))
            result = fixture.receive_body(io.BytesIO(body), count)
            self.assertEqual(result, {'bytesReceived': count, 'sha256': hashlib.sha256(body).hexdigest(), 'patternValid': True})
    def test_corruption_and_truncation(self):
        self.assertFalse(fixture.receive_body(io.BytesIO(b'\x01'), 1)['patternValid'])
        with self.assertRaises(EOFError): fixture.receive_body(io.BytesIO(b'\x00'), 2)
    def test_invalid_lengths(self):
        for count in (0, -1, fixture.MAX_UPLOAD+1):
            with self.assertRaises(ValueError): fixture.receive_body(io.BytesIO(), count)

class UploadHttpTests(unittest.TestCase):
    def test_receipt_and_rejection_are_audited(self):
        with tempfile.TemporaryDirectory() as directory:
            audit = Path(directory)/'audit.jsonl'
            server = fixture.create_server('127.0.0.1', 0, audit)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                body = bytes(i & 255 for i in range(12345))
                for payload, status in ((body, 200), (b'wrong', 422)):
                    client = http.client.HTTPConnection(*server.server_address, timeout=5)
                    try:
                        client.request('POST', '/upload?attempt=test', body=payload)
                        response = client.getresponse()
                        self.assertEqual(response.status, status)
                        self.assertEqual(response.read().decode(), hashlib.sha256(payload).hexdigest())
                    finally:
                        client.close()
                records = [json.loads(line) for line in audit.read_text().splitlines()]
                self.assertEqual([r['status'] for r in records], [200, 422])
                self.assertEqual(records[0]['bytesReceived'], len(body))
                self.assertEqual(records[0]['peer'], '127.0.0.1')
                self.assertEqual(records[0]['sha256'], hashlib.sha256(body).hexdigest())
                self.assertFalse(records[1]['patternValid'])
            finally:
                server.shutdown()
                thread.join(5)
                server.server_close()

if __name__ == '__main__': unittest.main()
