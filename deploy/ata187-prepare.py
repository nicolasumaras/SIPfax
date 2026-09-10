#!/usr/bin/env python3
"""Prepare an ATA187 modem call while FreePBX holds the device call lock."""
import fcntl
import json
import re
import signal
import sys
import syslog
import time
from pathlib import Path

SETTINGS = (
    'nom_delay 80', 'min_delay 80', 'adaptive_playout off', 'vad off',
    'ec off', 'tone_detect off', 'vp_concealment NONE',
)
EXPECTED = {
    'nominal': r'Playout nominal delay = 80 \(msec\)',
    'minimum': r'Playout minimum delay = 80 \(msec\)',
    'adaptive': r'Adaptive Playout = Disabled',
    'concealment': r'Packet loss conceal:\s+NONE',
    'vad': r'VAD = Disabled',
    'ec': r'EC\s+= Disabled',
    'tone': r'Tone detect = Disabled',
}


def profile_matches(response):
    response = response.replace('\r', '')
    return all(re.search(r'^\s*' + value + r'\s*$', response, re.M)
               for value in EXPECTED.values())


class Console:
    def __init__(self, channel):
        self.channel = channel
        channel.settimeout(1)
        self.read(r'(?m)^[^\r\n]*# ')
        self.command('set mxp')

    def read(self, prompt, echo=None):
        data = bytearray()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                chunk = self.channel.recv(32768)
            except TimeoutError:
                continue
            if not chunk:
                raise RuntimeError('CLI closed')
            data.extend(chunk)
            if len(data) > 1_048_576:
                raise RuntimeError('CLI response too large')
            text = data.decode(errors='replace')
            if re.search(prompt, text) and (echo is None or echo in text):
                return text
        raise TimeoutError('CLI prompt missing')

    def command(self, command):
        self.channel.sendall(command + '\n')
        response = self.read(r'MXP command --> ', command)
        if (command.startswith('set coding') or command == 'activate') and not re.search(
                r'^OK$', response.replace('\r', ''), re.M):
            raise RuntimeError('Setting rejected')
        return response

    def prepare(self):
        changed = not profile_matches(self.command('show coding 2'))
        if changed:
            for setting in SETTINGS:
                self.command('set coding 2 ' + setting)
        # "show coding" reads the pending profile, not the active DSP copy.
        # Activate even when pending values match. The caller must exclude
        # another active ATA call because this rebuilds both FXS channels.
        self.command('activate')
        if not profile_matches(self.command('show coding 2')):
            raise RuntimeError('Profile readback mismatch')
        return changed


def deadline_expired(signum, frame):
    raise TimeoutError('Preparation deadline exceeded')


def main():
    import paramiko  # Separate import permits console tests without SSH dependencies.
    started = time.monotonic()
    signal.signal(signal.SIGALRM, deadline_expired)
    signal.alarm(18)
    client = paramiko.SSHClient()
    syslog.openlog('sipfax-ata-prepare')
    try:
        config = json.loads(Path('/etc/sipfax/ata187.json').read_text())
        with open('/var/lib/sipfax-ata/prepare.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            client.load_host_keys('/etc/sipfax/ata187_known_hosts')
            client.set_missing_host_key_policy(paramiko.RejectPolicy())
            for attempt in range(2):
                try:
                    client.connect(config['host'], username=config['username'],
                                   password=config['password'], allow_agent=False,
                                   look_for_keys=False, timeout=5, auth_timeout=5,
                                   banner_timeout=5)
                    break
                except paramiko.ssh_exception.NoValidConnectionsError:
                    if attempt:
                        raise
                    client.close()
                    time.sleep(0.5)  # Dropbear can briefly refuse a new session.
            changed = Console(client.invoke_shell()).prepare()
        result = {'status': 'ready', 'changed': changed, 'activated': True,
                  'elapsedMs': round((time.monotonic() - started) * 1000)}
        print(json.dumps(result), flush=True)
        syslog.syslog(syslog.LOG_INFO, json.dumps(result))
        return 0
    except Exception as error:
        # Never echo credentials, console output or exception text into call logs.
        result = {'status': 'failed', 'errorType': type(error).__name__}
        print(json.dumps(result), flush=True)
        syslog.syslog(syslog.LOG_ERR, json.dumps(result))
        return 1
    finally:
        client.close()
        signal.alarm(0)


if __name__ == '__main__':
    sys.exit(main())
