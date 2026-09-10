import importlib.util
from pathlib import Path
import unittest
from unittest import mock
from contextlib import ExitStack, redirect_stdout
import io
import types

spec = importlib.util.spec_from_file_location(
    'ata_prepare', Path(__file__).resolve().parents[2] / 'deploy/ata187-prepare.py')
ata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ata)
PROFILE = '''Playout nominal delay = 80 (msec)
Playout minimum delay = 80 (msec)
Adaptive Playout = Disabled
Packet loss conceal:    NONE
VAD = Disabled
EC                   = Disabled
Tone detect = Disabled
'''


class FakeChannel:
    def __init__(self, profile=PROFILE, reject=False):
        self.parts = [b'Orthrus-sip# ']
        self.commands = []
        self.profile = profile
        self.reject = reject

    def settimeout(self, timeout):
        pass

    def sendall(self, data):
        command = data.strip()
        self.commands.append(command)
        if command == 'show coding 2':
            text = self.profile
        else:
            text = 'ERROR' if self.reject and command.startswith('set coding') else 'OK'
            if command == 'set coding 2 vp_concealment NONE':
                self.profile = PROFILE
        response = (command + '\r\n' + text + '\r\nMXP command --> ').encode()
        self.parts.extend([response[:11], response[11:-4], response[-4:]])

    def recv(self, length):
        return self.parts.pop(0) if self.parts else b''


class PreparationTests(unittest.TestCase):
    def test_exact_tone_field_required(self):
        wrong = PROFILE.replace('Tone detect = Disabled',
                                'Tone detect = Enabled\nV.18 Tone detect = Disabled')
        self.assertFalse(ata.profile_matches(wrong))

    def test_missing_or_wrong_field_rejected(self):
        self.assertFalse(ata.profile_matches(PROFILE.replace('NONE', 'G711A1')))
        self.assertFalse(ata.profile_matches(PROFILE.replace('VAD = Disabled', '')))

    def test_matching_pending_profile_still_requires_activation(self):
        channel = FakeChannel()
        self.assertFalse(ata.Console(channel).prepare())
        self.assertEqual(channel.commands, ['set mxp', 'show coding 2', 'activate', 'show coding 2'])

    def test_fragmented_prompts_update_then_activate(self):
        channel = FakeChannel(PROFILE.replace('NONE', 'G711A1'))
        self.assertTrue(ata.Console(channel).prepare())
        self.assertEqual(channel.commands[-2], 'activate')
        self.assertEqual(channel.commands[-1], 'show coding 2')

    def test_setting_rejection_aborts(self):
        channel = FakeChannel('defaults', reject=True)
        with self.assertRaises(RuntimeError):
            ata.Console(channel).prepare()
        self.assertEqual(len(channel.commands), 3)


    def run_main(self, failures):
        class Refused(Exception):
            pass
        client = mock.Mock()
        client.connect.side_effect = [Refused('secret-value')] * failures + [None]
        client.invoke_shell.return_value = FakeChannel()
        dependency = types.SimpleNamespace(
            SSHClient=lambda: client, RejectPolicy=object,
            ssh_exception=types.SimpleNamespace(NoValidConnectionsError=Refused))
        out = io.StringIO()
        with ExitStack() as stack:
            stack.enter_context(mock.patch.dict('sys.modules', paramiko=dependency))
            stack.enter_context(mock.patch.object(ata.Path, 'read_text', return_value=
                '{"host":"example","username":"user","password":"secret-value"}'))
            stack.enter_context(mock.patch.object(ata, 'open', mock.mock_open(), create=True))
            for target in ['fcntl.flock', 'signal.signal', 'signal.alarm',
                           'syslog.openlog', 'syslog.syslog', 'time.sleep']:
                owner, name = target.split('.')
                stack.enter_context(mock.patch.object(getattr(ata, owner), name))
            stack.enter_context(redirect_stdout(out))
            result = ata.main()
        return result, out.getvalue(), client

    def test_transient_connection_refusal_retried_once(self):
        result, output, client = self.run_main(1)
        self.assertEqual(result, 0)
        self.assertEqual(client.connect.call_count, 2)
        self.assertNotIn('secret-value', output)

    def test_persistent_refusal_fails_without_secret_logging(self):
        result, output, client = self.run_main(2)
        self.assertEqual(result, 1)
        self.assertEqual(client.connect.call_count, 2)
        self.assertNotIn('secret-value', output)
        self.assertNotIn('ready', output)


if __name__ == '__main__':
    unittest.main()
