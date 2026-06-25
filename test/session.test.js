import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseRtpPacket } from '../src/media.js';
import { buildHealth, renderFreePbxPjsip, renderMetrics } from '../src/operator.js';
import { AddressPool, EgressPolicy, PppCredentialStore, PppSessionController } from '../src/ppp.js';
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

test('egress policy allows public internet and blocks private destinations by default', () => {
  const policy = new EgressPolicy({ clientCidr: '10.70.0.0/24', outboundInterface: 'eth0' });

  assert.equal(policy.allows({ destination: '8.8.8.8', protocol: 'udp', destinationPort: 53 }), true);
  assert.equal(policy.allows({ destination: '192.168.1.1', protocol: 'tcp', destinationPort: 443 }), false);
  assert.equal(policy.allows({ destination: '203.0.113.20', protocol: 'tcp', destinationPort: 443 }), false);
  assert.match(policy.firewallRules().join('\n'), /MASQUERADE/);
  assert.match(policy.firewallRules().join('\n'), /-d 0\.0\.0\.0\/0 -o eth0 -j ACCEPT/);
  assert.match(policy.firewallRules().join('\n'), /-d 192\.168\.0\.0\/16 -j REJECT/);
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
