import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import dgram from 'node:dgram';
import { EventEmitter } from 'node:events';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  buildRtpPacket,
  ExternalModemProcessBackend,
  G711_CODECS,
  InProcessDialupTerminator,
  ModemAnswerToneSource,
  ModemBridge,
  parseRtpPacket
} from '../src/media.js';
import { buildHealth, OperatorHttpServer, renderFreePbxPjsip, renderMetrics } from '../src/operator.js';
import { AddressPool, EgressPolicy, PppCredentialStore, PppSessionController } from '../src/ppp.js';
import { buildPppdArgs, PppdSupervisor, renderChapSecrets } from '../src/pppd-supervisor.js';
import { SipFaxServer } from '../src/server.js';
import { SingleSessionManager } from '../src/session.js';
import { parseSdpOffer } from '../src/sdp.js';
import { buildResponse, parseSipMessage } from '../src/sip.js';

test('SDP parser accepts static G.711 payloads only when offered', () => {
  const offer = parseSdpOffer([
    'v=0',
    'o=ata 1 1 IN IP4 192.0.2.10',
    's=-',
    'c=IN IP4 192.0.2.10',
    't=0 0',
    'm=audio 18000 RTP/AVP 18 0 8',
    'a=rtpmap:18 G729/8000',
    'a=rtpmap:0 PCMU/8000',
    'a=rtpmap:8 PCMA/8000'
  ].join('\r\n'));

  assert.deepEqual(
    offer.codecs.map((codec) => codec.payloadType),
    [18, 0, 8]
  );
});

test('single-session manager starts one G.711 call and rejects concurrent INVITEs', () => {
  const manager = new SingleSessionManager({ publicHost: '198.51.100.5', localRtpPort: 40000 });
  const invite = parseSipMessage(makeInvite({ callId: 'call-1', payloads: '0 8' }));

  const first = manager.startFromInvite(invite);
  assert.equal(first.accepted, true);
  assert.equal(first.session.codec.name, 'PCMU');
  assert.equal(manager.activeSession.state, 'ringing');

  const second = manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'call-2', payloads: '0' })));
  assert.equal(second.accepted, false);
  assert.equal(second.statusCode, 486);
});

test('single-session manager rejects non-G.711 offers', () => {
  const manager = new SingleSessionManager({ publicHost: '198.51.100.5', localRtpPort: 40000 });
  const result = manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'call-3', payloads: '18' })));

  assert.equal(result.accepted, false);
  assert.equal(result.statusCode, 488);
});

test('ACK establishes and BYE frees the single-session slot', () => {
  const manager = new SingleSessionManager({ publicHost: '198.51.100.5', localRtpPort: 40000 });
  manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'call-4', payloads: '8' })));

  assert.equal(manager.acknowledge('call-4'), true);
  assert.equal(manager.activeSession.state, 'established');
  assert.equal(manager.activeSession.ppp.state, 'awaiting-auth');
  assert.equal(manager.terminate('call-4'), true);
  assert.equal(manager.activeSession, null);
});

test('PPP authentication assigns client address and DNS for an established call', () => {
  const ppp = new PppSessionController({
    credentials: new PppCredentialStore([{ username: 'fax', password: 'secret' }]),
    addressPool: new AddressPool({ cidr: '10.70.0.0/30' }),
    dnsServers: ['1.1.1.1']
  });
  const manager = new SingleSessionManager({
    publicHost: '198.51.100.5',
    localRtpPort: 40000,
    ppp
  });

  manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'call-ppp', payloads: '0' })));
  manager.acknowledge('call-ppp');

  const rejected = manager.authenticatePpp('call-ppp', { username: 'fax', password: 'wrong' });
  assert.equal(rejected.authenticated, false);
  assert.equal(rejected.reason, 'invalid-credentials');

  const accepted = manager.authenticatePpp('call-ppp', { username: 'fax', password: 'secret' });
  assert.equal(accepted.authenticated, true);
  assert.equal(accepted.session.lease.localAddress, '10.70.0.1');
  assert.equal(accepted.session.lease.clientAddress, '10.70.0.2');
  assert.deepEqual(accepted.session.dnsServers, ['1.1.1.1']);
  assert.equal(manager.diagnostics().ppp.addressPool.activeLeases, 1);

  manager.terminate('call-ppp');
  assert.equal(ppp.diagnostics().addressPool.activeLeases, 0);
});

test('PPP controller starts and stops pppd supervisor from leased session state', () => {
  const starts = [];
  const stops = [];
  const supervisor = {
    start(options) {
      starts.push(options);
      return {
        state: 'starting',
        localAddress: options.lease.localAddress,
        clientAddress: options.lease.clientAddress,
        dnsServers: options.dnsServers,
        interfaceName: null
      };
    },
    stop(callId) {
      stops.push(callId);
      return true;
    },
    diagnostics() {
      return { activeSessions: starts.length - stops.length };
    }
  };
  const ppp = new PppSessionController({
    credentials: new PppCredentialStore([{ username: 'fax', password: 'secret' }]),
    addressPool: new AddressPool({ cidr: '10.80.0.0/30' }),
    dnsServers: ['9.9.9.9'],
    pppdSupervisor: supervisor
  });

  ppp.begin('call-pty');
  assert.equal(ppp.startPppd('call-pty', { slavePath: '/dev/pts/7' }), true);
  assert.equal(starts.length, 1);
  assert.equal(starts[0].slavePath, '/dev/pts/7');
  assert.equal(starts[0].lease.localAddress, '10.80.0.1');
  assert.equal(starts[0].lease.clientAddress, '10.80.0.2');
  assert.deepEqual(starts[0].dnsServers, ['9.9.9.9']);
  assert.equal(ppp.snapshot('call-pty').state, 'pppd-starting');
  assert.equal(ppp.diagnostics().addressPool.activeLeases, 1);

  ppp.terminate('call-pty');
  assert.deepEqual(stops, ['call-pty']);
  assert.equal(ppp.diagnostics().addressPool.activeLeases, 0);
});

test('egress policy allows public internet and blocks private destinations by default', () => {
  const policy = new EgressPolicy({ clientCidr: '10.70.0.0/24', outboundInterface: 'eth0' });

  assert.equal(policy.allows({ destination: '8.8.8.8', protocol: 'udp', destinationPort: 53 }), true);
  assert.equal(policy.allows({ destination: '192.168.1.1', protocol: 'tcp', destinationPort: 443 }), false);
  assert.equal(policy.allows({ destination: '203.0.113.20', protocol: 'tcp', destinationPort: 443 }), false);
  assert.match(policy.firewallRules().join('\n'), /MASQUERADE/);
  assert.match(policy.firewallRules().join('\n'), /-d 0\.0\.0\.0\/0 -o eth0 -j ACCEPT/);
  assert.match(policy.firewallRules().join('\n'), /-d 192\.168\.0\.0\/16 -j REJECT/);
});

test('egress policy renders nftables rules and per-call descriptors', () => {
  const policy = new EgressPolicy({
    clientCidr: '10.70.0.0/24',
    outboundInterface: 'eth0',
    operatorUrl: ''
  });
  const descriptor = policy.leaseDescriptor({
    callId: 'call:nft',
    lease: { localAddress: '10.70.0.1', clientAddress: '10.70.0.2' }
  });

  assert.match(descriptor.nft.up.join('\n'), /add table inet sipfax_call_nft/);
  assert.match(descriptor.nft.up.join('\n'), /oifname "eth0" masquerade/);
  assert.deepEqual(descriptor.nft.down, [
    'delete table ip sipfax_nat_call_nft',
    'delete table inet sipfax_call_nft'
  ]);
  assert.match(descriptor.iptables.down.join('\n'), /iptables -D FORWARD/);
});

test('operator HTTP accepts PPP egress diagnostics events', async () => {
  const operator = new OperatorHttpServer({
    host: '127.0.0.1',
    port: 0,
    diagnostics: () => ({
      sip: { listening: true },
      rtp: { listening: true },
      ppp: { configuredUsers: 1 },
      sessions: { active: 0, limit: 1 },
      media: {},
      metrics: {}
    }),
    freepbx: { serverHost: '127.0.0.1', sipPort: 5060 }
  });
  await operator.start();
  const { port } = operator.server.address();

  try {
    const post = await fetch(`http://127.0.0.1:${port}/ppp/events`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ state: 'ip-up', callId: 'call-egress', interfaceName: 'ppp0' })
    });
    assert.equal(post.status, 202);

    const events = await fetch(`http://127.0.0.1:${port}/ppp/events`).then((response) => response.json());
    assert.equal(events.events.length, 1);
    assert.equal(events.events[0].state, 'ip-up');
    assert.equal(events.events[0].callId, 'call-egress');
  } finally {
    await operator.stop();
  }
});

test('SIP 200 OK answer carries SDP and dialog headers', () => {
  const request = parseSipMessage(makeInvite({ callId: 'call-5', payloads: '0' }));
  const response = buildResponse(request, 200, 'OK', {
    toTag: 'server-tag',
    body: 'v=0\r\n',
    headers: { 'Content-Type': 'application/sdp' }
  });

  assert.match(response, /^SIP\/2.0 200 OK/);
  assert.match(response, /To: <sip:sipfax@example.test>;tag=server-tag/);
  assert.match(response, /Content-Type: application\/sdp/);
  assert.match(response, /Content-Length: 5/);
});

test('RTP parser extracts static payload type and payload bytes', () => {
  const packet = Buffer.from([
    0x80, 0x00, 0x12, 0x34, 0x00, 0x00, 0x03, 0xe8, 0xaa, 0xbb, 0xcc, 0xdd,
    0xff, 0xfe, 0xfd
  ]);

  const parsed = parseRtpPacket(packet);
  assert.equal(parsed.payloadType, 0);
  assert.equal(parsed.sequenceNumber, 0x1234);
  assert.equal(parsed.timestamp, 1000);
  assert.deepEqual([...parsed.payload], [0xff, 0xfe, 0xfd]);
});

test('modem bridge hands inbound G.711 payload bytes to the downstream modem path', () => {
  const writes = [];
  const modem = {
    writeInboundAudio(payload, metadata) {
      writes.push({ payload, metadata });
    }
  };
  const bridge = new ModemBridge({ modem });
  bridge.setSessionCodec(G711_CODECS.get(0));
  const sourcePayload = Buffer.from([0x7f, 0x80, 0x81]);

  bridge.acceptFrame({
    payloadType: 0,
    sequenceNumber: 7,
    timestamp: 160,
    payload: sourcePayload
  });
  sourcePayload[0] = 0x00;

  assert.equal(writes.length, 1);
  assert.deepEqual([...writes[0].payload], [0x7f, 0x80, 0x81]);
  assert.equal(writes[0].metadata.codec.name, 'PCMU');
  assert.equal(writes[0].metadata.sequenceNumber, 7);
  assert.deepEqual(bridge.diagnostics(), {
    codec: 'PCMU',
    modemAttached: true,
    modem: null,
    framesIn: 1,
    framesOut: 0,
    audioBytesIn: 3,
    audioBytesOut: 0
  });
});

test('modem bridge accepts outbound modem audio for RTP send without transcoding', () => {
  const bridge = new ModemBridge();
  bridge.setSessionCodec(G711_CODECS.get(8));
  const emitted = [];
  bridge.on('outbound-audio', (audio) => emitted.push(audio));
  const sourcePayload = Buffer.from([0xd5, 0xd4, 0xd3, 0xd2]);

  const audio = bridge.acceptOutboundAudio(sourcePayload, { timestampIncrement: 160, marker: true });
  sourcePayload[1] = 0x00;

  assert.equal(emitted.length, 1);
  assert.equal(audio, emitted[0]);
  assert.equal(audio.payloadType, 8);
  assert.equal(audio.timestampIncrement, 160);
  assert.equal(audio.marker, true);
  assert.deepEqual([...audio.payload], [0xd5, 0xd4, 0xd3, 0xd2]);
  assert.equal(bridge.diagnostics().audioBytesOut, 4);
});

test('modem bridge subscribes to outbound audio emitted by the attached modem', () => {
  const modem = new EventEmitter();
  const bridge = new ModemBridge({ modem });
  bridge.setSessionCodec(G711_CODECS.get(0));
  const emitted = [];
  bridge.on('outbound-audio', (audio) => emitted.push(audio));

  modem.emit('outbound-audio', Buffer.from([0xff, 0xfe]), { timestampIncrement: 160 });

  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].payloadType, 0);
  assert.equal(emitted[0].timestampIncrement, 160);
  assert.deepEqual([...emitted[0].payload], [0xff, 0xfe]);
  assert.equal(bridge.diagnostics().framesOut, 1);
});

test('external modem process backend exchanges framed G.711 payloads with a real process', async () => {
  const backend = new ExternalModemProcessBackend({
    command: process.execPath,
    args: [
      '-e',
      [
        'let buffer = Buffer.alloc(0);',
        'process.stdin.on("data", (chunk) => {',
        '  buffer = Buffer.concat([buffer, chunk]);',
        '  while (buffer.length >= 2) {',
        '    const length = buffer.readUInt16BE(0);',
        '    if (buffer.length < length + 2) return;',
        '    const payload = Buffer.from(buffer.subarray(2, length + 2));',
        '    buffer = buffer.subarray(length + 2);',
        '    payload[0] = Number(process.env.SIPFAX_MODEM_PAYLOAD_TYPE);',
        '    const header = Buffer.alloc(2);',
        '    header.writeUInt16BE(payload.length, 0);',
        '    process.stdout.write(Buffer.concat([header, payload]));',
        '  }',
        '});'
      ].join('')
    ]
  });

  try {
    const outbound = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timed out waiting for modem process output')), 500);
      backend.once('outbound-audio', (payload, metadata) => {
        clearTimeout(timeout);
        resolve({ payload, metadata });
      });
    });

    backend.setSessionCodec(G711_CODECS.get(8));
    assert.equal(backend.writeInboundAudio(Buffer.from([0xd5, 0x22, 0x33])), true);

    const emitted = await outbound;
    assert.deepEqual([...emitted.payload], [8, 0x22, 0x33]);
    assert.equal(emitted.metadata.payloadType, 8);
    assert.equal(emitted.metadata.timestampIncrement, 3);
    assert.equal(backend.diagnostics().running, true);
  } finally {
    backend.stop();
  }
});

test('external modem process backend opens fd 3 and emits parsed control events', async () => {
  const backend = new ExternalModemProcessBackend({
    command: process.execPath,
    args: [
      '-e',
      [
        'const fs = require("node:fs");',
        'const control = fs.createWriteStream(null, { fd: 3 });',
        'control.write(JSON.stringify({ state: "data-mode", modulation: "V.21", startMode: "v8", v8Status: "failed", v8StatusCode: 2, v8Modulations: 0, lastEvent: "test-control" }) + "\\n");',
        'setTimeout(() => process.exit(0), 10);'
      ].join('')
    ]
  });

  try {
    const controlEvent = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timed out waiting for modem control event')), 500);
      backend.once('backend-control', (event) => {
        clearTimeout(timeout);
        resolve(event);
      });
    });

    backend.setSessionCodec(G711_CODECS.get(0));

    assert.deepEqual(await controlEvent, {
      state: 'data-mode',
      modulation: 'V.21',
      startMode: 'v8',
      v8Status: 'failed',
      v8StatusCode: 2,
      v8Modulations: 0,
      lastEvent: 'test-control'
    });
    const diagnostics = backend.diagnostics();
    assert.equal(diagnostics.state, 'data-mode');
    assert.equal(diagnostics.modulation, 'V.21');
    assert.equal(diagnostics.startMode, 'v8');
    assert.equal(diagnostics.v8Status, 'failed');
    assert.equal(diagnostics.v8StatusCode, 2);
    assert.equal(diagnostics.v8Modulations, 0);
    assert.equal(diagnostics.lastEvent, 'test-control');
    assert.match(diagnostics.lastEventAt, /^\d{4}-\d{2}-\d{2}T/);
  } finally {
    backend.stop();
  }
});

test('default modem answer-tone source emits negotiated G.711 handshake frames', () => {
  const source = new ModemAnswerToneSource();
  const emitted = [];
  source.on('outbound-audio', (payload, metadata) => emitted.push({ payload, metadata }));
  source.setSessionCodec(G711_CODECS.get(8));

  source.writeInboundAudio();
  source.stop();

  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].payload.length, 160);
  assert.equal(emitted[0].metadata.payloadType, 8);
  assert.equal(emitted[0].metadata.timestampIncrement, 160);
  assert.notEqual(new Set(emitted[0].payload).size, 1);
});

test('in-process dial-up terminator emits ANSam frames and exposes negotiation state', () => {
  const terminator = new InProcessDialupTerminator({ trainingFramesRequired: 2 });
  const emitted = [];
  const states = [];
  terminator.on('outbound-audio', (payload, metadata) => emitted.push({ payload, metadata }));
  terminator.on('protocol-state', (event) => states.push(event));

  terminator.setSessionCodec(G711_CODECS.get(0));
  assert.equal(terminator.diagnostics().state, 'answer-tone');
  assert.equal(terminator.writeInboundAudio(Buffer.alloc(160, 0x00)), true);
  assert.equal(terminator.writeInboundAudio(Buffer.alloc(160, 0x00)), true);
  terminator.stop();

  assert.equal(emitted.length >= 1, true);
  assert.equal(emitted[0].payload.length, 160);
  assert.equal(emitted[0].metadata.payloadType, 0);
  assert.equal(emitted[0].metadata.timestampIncrement, 160);
  assert.equal(emitted[0].metadata.dialupState, 'answer-tone');
  assert.equal(terminator.diagnostics().state, 'v8-training');
  assert.equal(terminator.diagnostics().framesIn, 2);
  assert.equal(terminator.diagnostics().lastInboundEnergy >= terminator.diagnostics().inboundEnergyThreshold, true);
  assert.deepEqual(
    states.map((event) => event.state),
    ['answer-tone', 'v8-training']
  );
});

test('in-process dial-up terminator stops at carrier training without synthetic PPP frames', () => {
  const terminator = new InProcessDialupTerminator({
    trainingFramesRequired: 2,
    carrierFramesRequired: 2
  });
  const emitted = [];
  const states = [];
  terminator.on('outbound-audio', (payload, metadata) => emitted.push({ payload, metadata }));
  terminator.on('protocol-state', (event) => states.push(event));

  terminator.setSessionCodec(G711_CODECS.get(0));
  for (let index = 0; index < 4; index += 1) {
    assert.equal(terminator.writeInboundAudio(Buffer.alloc(160, 0x00)), true);
  }
  assert.equal(terminator.diagnostics().state, 'carrier-training');

  terminator.emitFrame();
  terminator.emitFrame();
  terminator.stop();

  assert.deepEqual(
    states.map((event) => event.state),
    ['answer-tone', 'v8-training', 'carrier-training']
  );
  assert.equal(terminator.diagnostics().state, 'carrier-training');
  assert.equal(terminator.diagnostics().carrierHits, 2);
  assert.equal('pppProbeFramesOut' in terminator.diagnostics(), false);
  assert.equal('pppProbeBytes' in terminator.diagnostics(), false);
  assert.equal(emitted.some((frame) => frame.metadata.dialupState === 'carrier-training'), true);
  assert.equal(emitted.some((frame) => frame.metadata.dialupState === 'ppp-lcp-probe'), false);
  assert.equal(emitted.at(-1).payload.length, 160);
  assert.notEqual(new Set(emitted.at(-1).payload).size, 1);
});

test('pppd supervisor renders chap-secrets with restrictive permissions', () => {
  const dir = mkdtempSync(join(tmpdir(), 'sipfax-pppd-test-'));
  const secretsPath = join(dir, 'chap-secrets-123');
  const credentials = new PppCredentialStore([
    { username: 'fax', password: 'secret' },
    { username: 'quote"user', password: 'slash\\secret' }
  ]);

  renderChapSecrets(credentials, secretsPath);

  assert.equal(readFileSync(secretsPath, 'utf8'), [
    '"fax" * "secret" *',
    '"quote\\"user" * "slash\\\\secret" *',
    ''
  ].join('\n'));
  assert.equal(statSync(secretsPath).mode & 0o777, 0o600);
});

test('pppd supervisor builds required daemon options', () => {
  const args = buildPppdArgs({
    slavePath: '/dev/pts/9',
    lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' },
    dnsServers: ['1.1.1.1', '9.9.9.9'],
    authProtocol: 'chap',
    secretsPath: '/tmp/chap-secrets-111',
    notifyScript: '/usr/lib/sipfax/ppp-notify',
    callId: 'call-pppd'
  });

  assert.deepEqual(args.slice(0, 6), [
    '/dev/pts/9',
    'nodetach',
    'nodefaultroute',
    'noccp',
    'require-chap',
    '10.64.0.1:10.64.0.2'
  ]);
  assert.equal(args.includes('ms-dns'), true);
  assert.equal(args.includes('chap-secrets'), true);
  assert.equal(args.includes('/tmp/chap-secrets-111'), true);
  assert.equal(args.includes('ip-up-script'), true);
  assert.equal(args.includes('ip-down-script'), true);
  assert.equal(args.includes('lcp-echo-interval'), true);
  assert.equal(args.includes('lcp-max-configure'), true);
  assert.equal(args.includes('ipcp-max-configure'), true);
});

test('pppd supervisor writes secrets, accepts notify events, and cleans shutdown', () => {
  const child = new EventEmitter();
  child.pid = 4242;
  child.stdout = new EventEmitter();
  child.stderr = new EventEmitter();
  child.killed = false;
  child.kill = (signal) => {
    child.killed = signal;
  };
  const spawns = [];
  const removed = [];
  const secretsDir = mkdtempSync(join(tmpdir(), 'sipfax-secrets-'));
  const supervisor = new PppdSupervisor({
    command: '/usr/sbin/pppd',
    tempDir: tmpdir(),
    secretsDir,
    spawnProcess(command, args, options) {
      spawns.push({ command, args, options });
      return child;
    },
    cleanup(path) {
      removed.push(path);
    }
  });
  const credentials = new PppCredentialStore([{ username: 'fax', password: 'secret' }]);

  const started = supervisor.start({
    callId: 'call-supervisor',
    slavePath: '/dev/pts/3',
    lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' },
    dnsServers: ['1.1.1.1'],
    credentials
  });

  assert.equal(spawns[0].command, '/usr/sbin/pppd');
  assert.equal(spawns[0].args[0], '/dev/pts/3');
  assert.equal(spawns[0].args.includes('nodetach'), true);
  assert.equal(started.pid, 4242);
  const secretsPath = join(secretsDir, 'chap-secrets');
  assert.equal(existsSync(secretsPath), true);
  assert.match(readFileSync(secretsPath, 'utf8'), /"fax" \* "secret" \*/);

  child.stdout.emit('data', Buffer.from('{"state":"IPCP-open","interfaceName":"ppp0"}\n'));
  const snapshot = supervisor.snapshot('call-supervisor');
  assert.equal(snapshot.state, 'ipcp-open');
  assert.equal(snapshot.interfaceName, 'ppp0');
  assert.equal(snapshot.localAddress, '10.64.0.1');
  assert.equal(snapshot.clientAddress, '10.64.0.2');

  assert.equal(supervisor.stop('call-supervisor'), true);
  assert.equal(child.killed, 'SIGTERM');
  assert.equal(removed.length, 1);
});

test('pppd supervisor writes egress lease descriptor before daemon start', () => {
  const child = new EventEmitter();
  child.pid = 5252;
  child.stdout = new EventEmitter();
  child.stderr = new EventEmitter();
  child.kill = () => {};
  const leaseDir = mkdtempSync(join(tmpdir(), 'sipfax-lease-dir-'));
  const secretsDir = mkdtempSync(join(tmpdir(), 'sipfax-secrets-'));
  const supervisor = new PppdSupervisor({
    command: '/usr/sbin/pppd',
    leaseDir,
    secretsDir,
    spawnProcess() {
      return child;
    }
  });
  const credentials = new PppCredentialStore([{ username: 'fax', password: 'secret' }]);

  const started = supervisor.start({
    callId: 'call-descriptor',
    slavePath: '/dev/pts/4',
    lease: { localAddress: '10.64.0.1', clientAddress: '10.64.0.2' },
    credentials,
    egressDescriptor: {
      callId: 'call-descriptor',
      outboundInterface: 'eth0',
      nft: { up: ['add table inet sipfax_call_descriptor'], down: ['delete table inet sipfax_call_descriptor'] },
      iptables: { up: [], down: [] }
    }
  });

  assert.equal(started.egressDescriptorPath, join(leaseDir, 'call-descriptor.json'));
  assert.equal(JSON.parse(readFileSync(started.egressDescriptorPath, 'utf8')).outboundInterface, 'eth0');
});

test('sipfax-egress-apply applies and rolls back nft rules across a PPP cycle', () => {
  const root = mkdtempSync(join(tmpdir(), 'sipfax-egress-helper-'));
  const mockBin = join(root, 'bin');
  const leaseDir = join(root, 'leases');
  const activeDir = join(root, 'active');
  const logPath = join(root, 'commands.log');
  mkdirSync(mockBin);
  mkdirSync(leaseDir);

  writeFileSync(join(mockBin, 'nft'), [
    '#!/bin/sh',
    'printf "nft %s\\n" "$*" >> "$SIPFAX_TEST_LOG"',
    'cat >> "$SIPFAX_TEST_LOG"'
  ].join('\n'));
  writeFileSync(join(mockBin, 'sysctl'), [
    '#!/bin/sh',
    'printf "sysctl %s\\n" "$*" >> "$SIPFAX_TEST_LOG"'
  ].join('\n'));
  chmodSync(join(mockBin, 'nft'), 0o755);
  chmodSync(join(mockBin, 'sysctl'), 0o755);

  const descriptor = new EgressPolicy({
    clientCidr: '10.88.0.0/24',
    outboundInterface: 'eth-test0',
    operatorUrl: ''
  }).leaseDescriptor({
    callId: 'call-cycle',
    lease: { localAddress: '10.88.0.1', clientAddress: '10.88.0.2' }
  });
  writeFileSync(join(leaseDir, 'call-cycle.json'), `${JSON.stringify(descriptor)}\n`);

  const env = {
    ...process.env,
    PATH: `${mockBin}:${process.env.PATH}`,
    SIPFAX_PPP_LEASE_DIR: leaseDir,
    SIPFAX_PPP_ACTIVE_DIR: activeDir,
    SIPFAX_TEST_LOG: logPath
  };
  const helper = join(process.cwd(), 'bin/sipfax-egress-apply');

  execFileSync(process.execPath, [helper, 'up', 'call-cycle', 'ppp0', '10.88.0.1', '10.88.0.2'], { env });
  execFileSync(process.execPath, [helper, 'down', 'call-cycle', 'ppp0', '10.88.0.1', '10.88.0.2'], { env });

  const log = readFileSync(logPath, 'utf8');
  assert.match(log, /sysctl -w net\.ipv4\.ip_forward=1/);
  assert.match(log, /sysctl -w net\.ipv4\.conf\.eth-test0\.forwarding=1/);
  assert.match(log, /add table inet sipfax_call_cycle/);
  assert.match(log, /delete table inet sipfax_call_cycle/);
  assert.match(log, /sysctl -w net\.ipv4\.ip_forward=0/);
});

test('in-process dial-up terminator clears state when codec is removed', () => {
  const terminator = new InProcessDialupTerminator();

  terminator.setSessionCodec(G711_CODECS.get(8));
  terminator.writeInboundAudio(Buffer.alloc(160, 0xd5));
  terminator.setSessionCodec(null);

  assert.equal(terminator.diagnostics().state, 'idle');
  assert.equal(terminator.diagnostics().running, false);
});

test('server runtime modem wiring sends outbound RTP after inbound media discovers the remote endpoint', async () => {
  const modem = new EventEmitter();
  modem.setSessionCodec = (codec) => {
    modem.codec = codec;
  };
  modem.writeInboundAudio = () => {
    modem.emit('outbound-audio', Buffer.from([0x21, 0x22, 0x23]), { timestampIncrement: 160 });
  };

  const server = new SipFaxServer({
    host: '127.0.0.1',
    publicHost: '127.0.0.1',
    sipPort: 0,
    rtpPort: 0,
    modem
  });
  const remote = dgram.createSocket('udp4');

  try {
    await server.start();
    server.rtpEndpoint.setSessionCodec(G711_CODECS.get(0));
    server.modemBridge.setSessionCodec(G711_CODECS.get(0));
    const received = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timed out waiting for outbound RTP')), 500);
      remote.once('message', (message) => {
        clearTimeout(timeout);
        resolve(message);
      });
    });

    await new Promise((resolve) => remote.bind(0, '127.0.0.1', resolve));
    const serverRtpPort = server.rtpEndpoint.socket.address().port;
    const inbound = buildRtpPacket({
      payloadType: 0,
      sequenceNumber: 1,
      timestamp: 160,
      ssrc: 0x01020304,
      payload: Buffer.from([0x7f, 0x80, 0x81])
    });
    remote.send(inbound, serverRtpPort, '127.0.0.1');

    const outbound = parseRtpPacket(await received);
    assert.equal(outbound.payloadType, 0);
    assert.deepEqual([...outbound.payload], [0x21, 0x22, 0x23]);
    assert.equal(server.modemBridge.diagnostics().framesOut, 1);
  } finally {
    await Promise.all([
      server.stop(),
      new Promise((resolve) => remote.close(resolve))
    ]);
  }
});

test('server modem control pty events start and stop pppd for the active call', () => {
  const starts = [];
  const stops = [];
  const ppp = new PppSessionController({
    credentials: new PppCredentialStore([{ username: 'fax', password: 'secret' }]),
    addressPool: new AddressPool({ cidr: '10.90.0.0/30' }),
    pppdSupervisor: {
      start(options) {
        starts.push(options);
        return { state: 'starting' };
      },
      stop(callId) {
        stops.push(callId);
        return true;
      },
      diagnostics() {
        return {};
      }
    }
  });
  const server = new SipFaxServer({
    host: '127.0.0.1',
    publicHost: '127.0.0.1',
    sipPort: 0,
    rtpPort: 0,
    ppp
  });

  server.sessions.startFromInvite(parseSipMessage(makeInvite({ callId: 'call-control', payloads: '0' })));
  server.sessions.acknowledge('call-control');
  server.handleModemControl({ event: 'pty-opened', slavePath: '/dev/pts/11' });
  server.handleModemControl({ event: 'pty-closed' });

  assert.equal(starts.length, 1);
  assert.equal(starts[0].slavePath, '/dev/pts/11');
  assert.deepEqual(stops, ['call-control']);
});

test('server with unavailable softmodem worker emits no synthetic outbound RTP', async () => {
  const modem = new ExternalModemProcessBackend({ command: '/definitely/not-installed/sipfax-softmodem' });
  const backendError = new Promise((resolve) => {
    modem.once('backend-error', resolve);
  });
  const server = new SipFaxServer({
    host: '127.0.0.1',
    publicHost: '127.0.0.1',
    sipPort: 0,
    rtpPort: 0,
    modem
  });
  const remote = dgram.createSocket('udp4');

  try {
    await server.start();
    server.rtpEndpoint.setSessionCodec(G711_CODECS.get(0));
    server.modemBridge.setSessionCodec(G711_CODECS.get(0));
    await backendError;

    const received = new Promise((resolve) => {
      const timeout = setTimeout(() => resolve(null), 150);
      remote.once('message', (message) => {
        clearTimeout(timeout);
        resolve(message);
      });
    });

    await new Promise((resolve) => remote.bind(0, '127.0.0.1', resolve));
    const serverRtpPort = server.rtpEndpoint.socket.address().port;
    remote.send(buildRtpPacket({
      payloadType: 0,
      sequenceNumber: 1,
      timestamp: 160,
      ssrc: 0x01020304,
      payload: Buffer.alloc(160, 0x7f)
    }), serverRtpPort, '127.0.0.1');

    assert.equal(await received, null);
    assert.equal(server.modemBridge.diagnostics().framesOut, 0);
    assert.equal(server.modemBridge.diagnostics().modem.type, 'external-process');
    assert.match(server.modemBridge.diagnostics().modem.lastError, /ENOENT/);
  } finally {
    await Promise.all([
      server.stop(),
      new Promise((resolve) => remote.close(resolve))
    ]);
  }
});

test('RTP builder carries outbound modem audio bytes as negotiated G.711 payload', () => {
  const packet = buildRtpPacket({
    payloadType: 8,
    sequenceNumber: 0x20,
    timestamp: 0x300,
    ssrc: 0x01020304,
    marker: true,
    payload: Buffer.from([0x11, 0x22, 0x33])
  });

  assert.equal(packet[1] & 0x80, 0x80);
  const parsed = parseRtpPacket(packet);
  assert.equal(parsed.payloadType, 8);
  assert.equal(parsed.sequenceNumber, 0x20);
  assert.equal(parsed.timestamp, 0x300);
  assert.equal(parsed.ssrc, 0x01020304);
  assert.deepEqual([...parsed.payload], [0x11, 0x22, 0x33]);
});

test('operator health reports degraded when PPP users are missing', () => {
  const health = buildHealth(makeDiagnostics({ configuredUsers: 0 }));

  assert.equal(health.status, 'degraded');
  assert.equal(health.checks.sipListening, true);
  assert.equal(health.checks.rtpListening, true);
  assert.equal(health.checks.pppUsersConfigured, false);
  assert.equal(health.checks.sessionCapacityAvailable, true);
});

test('operator metrics expose session, SIP, RTP, and PPP counters', () => {
  const metrics = renderMetrics(makeDiagnostics({
    activeSessions: 1,
    invitesAccepted: 2,
    invitesRejected: 1,
    rtpFramesAccepted: 12,
    rtpFramesDropped: 3,
    activeLeases: 1
  }));

  assert.match(metrics, /sipfax_active_sessions 1/);
  assert.match(metrics, /sipfax_session_limit 1/);
  assert.match(metrics, /sipfax_invites_total\{outcome="accepted"\} 2/);
  assert.match(metrics, /sipfax_invites_total\{outcome="rejected"\} 1/);
  assert.match(metrics, /sipfax_rtp_frames_total 12/);
  assert.match(metrics, /sipfax_rtp_dropped_total 3/);
  assert.match(metrics, /sipfax_ppp_active_leases 1/);
});

test('FreePBX PJSIP snippet keeps Asterisk out of the media feature path', () => {
  const config = renderFreePbxPjsip({
    serverHost: '198.51.100.5',
    sipPort: 5060,
    extension: '4900'
  });

  assert.match(config, /Route extension 4900/);
  assert.match(config, /allow=ulaw,alaw/);
  assert.match(config, /t38_udptl=no/);
  assert.match(config, /direct_media=no/);
  assert.match(config, /contact=sip:198\.51\.100\.5:5060/);
  assert.match(config, /max_contacts=1/);
});

function makeDiagnostics({
  activeSessions = 0,
  configuredUsers = 1,
  invitesAccepted = 0,
  invitesRejected = 0,
  rtpFramesAccepted = 0,
  rtpFramesDropped = 0,
  activeLeases = 0
} = {}) {
  return {
    sip: { listening: true },
    rtp: { listening: true },
    sessions: { active: activeSessions, limit: 1 },
    ppp: {
      configuredUsers,
      addressPool: { activeLeases },
      egress: {}
    },
    metrics: {
      invitesAccepted,
      invitesRejected,
      rtpFramesAccepted,
      rtpFramesDropped
    }
  };
}

function makeInvite({ callId, payloads }) {
  return [
    'INVITE sip:sipfax@example.test SIP/2.0',
    'Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-test',
    'From: <sip:ata@example.test>;tag=ata-tag',
    'To: <sip:sipfax@example.test>',
    `Call-ID: ${callId}`,
    'CSeq: 1 INVITE',
    'Contact: <sip:ata@192.0.2.10>',
    'Content-Type: application/sdp',
    '',
    'v=0',
    'o=ata 1 1 IN IP4 192.0.2.10',
    's=-',
    'c=IN IP4 192.0.2.10',
    't=0 0',
    `m=audio 18000 RTP/AVP ${payloads}`
  ].join('\r\n');
}
