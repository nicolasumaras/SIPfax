import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseRtpPacket } from '../src/media.js';
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
  assert.equal(manager.terminate('call-4'), true);
  assert.equal(manager.activeSession, null);
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
