#!/usr/bin/env python3
"""Receive bounded deterministic bulk uploads for PPP acceptance testing."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import threading
import time

MAX_UPLOAD = 1048576

def receive_body(stream, length):
    if not 1 <= length <= MAX_UPLOAD:
        raise ValueError('Content-Length must be between 1 and 1048576')
    digest = hashlib.sha256()
    received = 0
    valid = True
    while received < length:
        data = stream.read(min(4096, length-received))
        if not data:
            raise EOFError('Upload ended before Content-Length')
        valid = valid and all(value == ((received+i) & 255) for i, value in enumerate(data))
        digest.update(data)
        received += len(data)
    return {'bytesReceived': received, 'sha256': digest.hexdigest(), 'patternValid': valid}

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        started = time.monotonic()
        record = {'path': self.path, 'peer': self.client_address[0], 'startedUnix': time.time()}
        try:
            self.connection.settimeout(600)
            if self.path.split('?', 1)[0] != '/upload':
                raise ValueError('Use /upload')
            if self.headers.get('Transfer-Encoding'):
                raise ValueError('Use Content-Length, not transfer encoding')
            lengths = self.headers.get_all('Content-Length', [])
            if len(lengths) != 1:
                raise ValueError('One Content-Length header is required')
            record.update(receive_body(self.rfile, int(lengths[0])))
            status = 200 if record['patternValid'] else 422
            body = record['sha256'].encode('ascii')
        except (ValueError, EOFError, OSError) as error:
            record['error'] = str(error)
            status, body = 400, b'INCOMPLETE OR INVALID UPLOAD'
        record.update(status=status, elapsedSeconds=time.monotonic()-started)
        with self.server.audit_lock:
            with self.server.audit_path.open('a') as output:
                output.write(json.dumps(record, sort_keys=True)+'\n')
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.close_connection = True
        try:
            self.wfile.write(body)
        except OSError:
            pass

def create_server(bind, port, audit_path, freebind=False):
    server = ThreadingHTTPServer((bind, port), Handler, bind_and_activate=False)
    try:
        if freebind:
            # Linux IP_FREEBIND lets the fixture start before PPP creates its address.
            server.socket.setsockopt(socket.SOL_IP, 15, 1)
        server.audit_path, server.audit_lock = audit_path, threading.Lock()
        server.server_bind()
        server.server_activate()
        return server
    except Exception:
        server.server_close()
        raise


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bind', default='10.64.0.1')
    p.add_argument('--port', type=int, default=8084)
    p.add_argument('--audit', type=Path, required=True)
    p.add_argument('--freebind', action='store_true', help='Linux: bind before the PPP address exists')
    args = p.parse_args()
    with create_server(args.bind, args.port, args.audit, args.freebind) as server:
        server.serve_forever()

if __name__ == '__main__':
    main()
