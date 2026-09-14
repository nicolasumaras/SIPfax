#!/usr/bin/env python
"""Experimental ATA187 RFC2198 bridge. Python 2.7/3; Linux NFQUEUE."""
from __future__ import print_function
import argparse, collections, fcntl, grp, json, os, re, signal, socket, struct, subprocess, sys, threading, time

def checksum(data):
    if len(data) % 2: data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data)//2), data))
    while total >> 16: total = (total & 65535) + (total >> 16)
    return (~total) & 65535

class RedEncoder(object):
    def __init__(self, source, destination):
        self.addresses = socket.inet_aton(source) + socket.inet_aton(destination)
        self.registration = None
        self.previous = None

    def register(self, value):
        if value != self.registration:
            self.registration = value
            self.previous = None

    def packet(self, raw):
        b = bytearray(raw)
        if len(b) < 40 or b[0] != 0x45 or b[9] != 17: return raw
        if struct.unpack_from('!H', b, 6)[0] & 0x3fff: return raw
        if bytes(b[12:20]) != self.addresses: return raw
        src, dst, length = struct.unpack_from('!HHH', b, 20)
        if length != 180 or len(b) != 200: return raw
        r = bytes(b[28:]); rb = bytearray(r)
        if rb[0] != 128 or rb[1] & 127 != 0: return raw
        seq, stamp, ssrc = struct.unpack_from('!HII', r, 2)
        payload = r[12:]; key = (src, dst, ssrc)
        if not self.registration or dst != self.registration[1]: return raw
        old = self.previous
        extra = b'\0' + payload
        if old and old[0] == key and (seq-old[1]) & 65535 == 1 and (stamp-old[2]) & 0xffffffff == 160:
            extra = b'\x80' + struct.pack('!I', (160 << 10) | 160)[1:] + b'\0' + old[3] + payload
        self.previous = (key, seq, stamp, payload)
        header = bytearray(r[:12]); header[1] = (header[1] & 128) | 96
        b = b[:28] + header + bytearray(extra)
        struct.pack_into('!H', b, 2, len(b)); struct.pack_into('!H', b, 24, len(b)-20)
        b[10:12] = b'\0\0'; b[26:28] = b'\0\0'
        struct.pack_into('!H', b, 10, checksum(bytes(b[:20])))
        pseudo = bytes(b[12:20]) + b'\0\x11' + struct.pack('!H', len(b)-20)
        struct.pack_into('!H', b, 26, checksum(pseudo + bytes(b[20:])) or 65535)
        return bytes(b)

def attr(kind, data):
    length = 4 + len(data)
    return struct.pack('=HH', length, kind) + data + b'\0' * ((-length) & 3)

def messages(raw):
    pos = 0
    while pos + 16 <= len(raw):
        length, kind, flags, seq, pid = struct.unpack_from('=IHHII', raw, pos)
        if length < 16 or pos + length > len(raw): raise ValueError('Truncated netlink message')
        yield kind, seq, raw[pos+16:pos+length]
        pos += (length+3) & ~3

def attributes(raw):
    pos = 0; result = {}
    while pos + 4 <= len(raw):
        length, kind = struct.unpack_from('=HH', raw, pos)
        if length < 4 or pos + length > len(raw): raise ValueError('Truncated attribute')
        result[kind & 0x3fff] = raw[pos+4:pos+length]
        pos += (length+3) & ~3
    return result

class Queue(object):
    def __init__(self, number):
        self.number = number; self.seq = 0
        self.sock = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 12)
        self.sock.bind((0, 0)); self.sock.connect((0, 0)); self.sock.settimeout(3)
        self.send(2, attr(1, struct.pack('!BBH', 1, 0, socket.AF_INET)), ack=True)
        self.send(2, attr(2, struct.pack('!IB', 65535, 2)), ack=True)
        # NFQA_CFG_F_FAIL_OPEN: preserve ordinary audio if the queue is full.
        self.send(2, attr(4, struct.pack('!I', 1)) + attr(5, struct.pack('!I', 1)), ack=True)
    def send(self, kind, data, ack=False):
        self.seq += 1
        body = struct.pack('!BBH', 0, 0, self.number) + data
        header = struct.pack('=IHHII', 16+len(body), (3<<8)|kind, 1|(4 if ack else 0), self.seq, self.sock.getsockname()[0])
        self.sock.send(header+body)
        if ack:
            for typ, seq, msg in messages(self.sock.recv(65535)):
                if typ == 2 and seq == self.seq:
                    error = struct.unpack_from('=i', msg)[0]
                    if error: raise OSError(-error, os.strerror(-error))
                    return
            raise RuntimeError('Missing configuration ACK')
    def verdict(self, identity, payload, drop=False):
        self.send(1, attr(2, struct.pack('!I', 0 if drop else 1)+identity) + (b'' if drop or payload is None else attr(10, payload)))


def log(**fields):
    print(json.dumps(fields)); sys.stdout.flush()


def parse_destination(text, address):
    match = re.match(r'^Value: ([A-Za-z0-9_.-]{1,96})\|([^:]+):([0-9]{1,5})\s*$', text.strip())
    if not match or match.group(2) != address: return None
    port = int(match.group(3))
    if not 16384 <= port <= 65534: return None
    return (match.group(1), port)


class Control(threading.Thread):
    # Only this thread runs Asterisk commands. The RTP loop never waits for them.
    def __init__(self, path, destination):
        threading.Thread.__init__(self)
        self.daemon = True
        self.destination = destination
        self.registration = None
        self.alive = True
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        os.chown(path, 0, grp.getgrnam('asterisk').gr_gid)
        os.chmod(path, 0o660)
        self.sock.listen(4)
        self.sock.settimeout(0.5)

    def run(self):
        while self.alive:
            try: conn, _ = self.sock.accept()
            except socket.timeout: continue
            except socket.error: return
            try:
                conn.settimeout(2)
                command = conn.recv(32)
                if command != b'sync':
                    conn.sendall(b'ERROR\n'); continue
                raw = subprocess.check_output([
                    '/usr/bin/timeout', '1', '/usr/sbin/asterisk', '-rx',
                    'database get SIPFAXRED current'])
                value = parse_destination(raw.decode('ascii'), self.destination)
                self.registration = value
                log(event='registration', callId=value[0] if value else None,
                    port=value[1] if value else None)
                conn.sendall(b'ACTIVE\n' if value else b'IDLE\n')
            except Exception as error:
                self.registration = None
                log(event='control-error', errorType=type(error).__name__)
            finally:
                conn.close()


def sync(path, require_active=False):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(3)
    try:
        client.connect(path); client.sendall(b'sync')
        reply = client.recv(32)
        sys.stdout.write(reply.decode('ascii'))
        return 0 if reply == b'ACTIVE\n' or (reply == b'IDLE\n' and not require_active) else 1
    except (socket.error, UnicodeError): return 1
    finally: client.close()


def rule_for(destination):
    return ['-d', destination, '-p', 'udp', '--dport', '16384:65535',
            '-m', 'comment', '--comment', 'SIPFAX_RED', '-j', 'NFQUEUE',
            '--queue-num', '105', '--queue-bypass']


def remove_rule(rule):
    with open(os.devnull, 'w') as null:
        while subprocess.call(['/sbin/iptables', '-C', 'OUTPUT'] + rule, stderr=null) == 0:
            subprocess.check_call(['/sbin/iptables', '-D', 'OUTPUT'] + rule)


def serve(args):
    # The same lock protects normal startup and ExecStopPost cleanup.
    lock = open(args.socket + '.lock', 'a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    rule = rule_for(args.destination)
    remove_rule(rule)
    if os.path.exists(args.socket): os.unlink(args.socket)
    if args.cleanup: return 0
    q = None; control = None; counts = collections.Counter(); alive = [True]
    def stop(signum, frame): alive[0] = False
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    try:
        q = Queue(105); q.sock.settimeout(1)
        encoder = RedEncoder(args.source, args.destination)
        control = Control(args.socket, args.destination)
        subprocess.check_call(['/sbin/iptables', '-I', 'OUTPUT', '1'] + rule)
        control.start()
        log(event='ready', queue=105)
        while alive[0]:
            try: raw = q.sock.recv(131072)
            except socket.timeout: continue
            except socket.error as error:
                if error.errno == 4: continue
                raise
            for kind, seq, body in messages(raw):
                if kind != (3 << 8): continue
                a = attributes(body[4:]); identity = a[1][:4]; packet = a[10]
                encoder.register(control.registration)
                changed = encoder.packet(packet)
                modified = changed != packet
                counts['modified' if modified else 'unchanged'] += 1
                q.verdict(identity, changed if modified else None)
    finally:
        remove_rule(rule)
        if q: q.sock.close()
        if control:
            control.alive = False; control.sock.close(); control.join(3)
        if os.path.exists(args.socket): os.unlink(args.socket)
        log(event='stopped', counts=dict(counts))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='192.168.1.29')
    parser.add_argument('--destination', default='192.168.1.235')
    parser.add_argument('--socket', default='/run/sipfax-red/control')
    parser.add_argument('--sync', action='store_true')
    parser.add_argument('--cleanup', action='store_true')
    parser.add_argument('--require-active', action='store_true')
    args = parser.parse_args()
    # Validate addresses before using them as packet-filter arguments.
    socket.inet_aton(args.source); socket.inet_aton(args.destination)
    return sync(args.socket, args.require_active) if args.sync else serve(args)


if __name__ == '__main__':
    sys.exit(main())
