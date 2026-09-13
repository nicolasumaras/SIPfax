import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { EventEmitter, once } from 'node:events';
import { test } from 'node:test';
import { RtpEndpoint, RtpPortPool } from '../src/media.js';
import { SipFaxServer } from '../src/server.js';
import { parseSipMessage } from '../src/sip.js';

const invite = () => parseSipMessage('INVITE sip:fax@example SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.1:5060;branch=test\r\nCall-ID: bind-test\r\nFrom: <sip:a@x>;tag=ft\r\nTo: <sip:fax@x>\r\nCSeq: 1 INVITE\r\n\r\nv=0\r\no=- 1 1 IN IP4 192.0.2.1\r\ns=-\r\nc=IN IP4 192.0.2.1\r\nt=0 0\r\nm=audio 5004 RTP/AVP 0\r\n');

test('RTP bind conflict rejects startup without an unhandled socket error', { timeout: 2000 }, async () => {
  const holder = dgram.createSocket('udp4');
  holder.bind(0, '127.0.0.1'); await once(holder, 'listening');
  const endpoint = new RtpEndpoint({ host: '127.0.0.1', port: holder.address().port });
  try { await assert.rejects(endpoint.start(), { code: 'EADDRINUSE' }); }
  finally {
    try { await endpoint.stop(); } catch (error) { assert.equal(error.code, 'ERR_SOCKET_DGRAM_NOT_RUNNING'); }
    await new Promise(resolve => holder.close(resolve));
  }
});

for (const success of [true, false]) {
  test(`SIP waits for RTP readiness and ${success ? 'accepts' : 'rejects and frees the call'}`, async (t) => {
    t.mock.method(console, 'error', () => {});
    let ready, failed, stops = 0;
    const responses = [];
    const server = new SipFaxServer({ host: '127.0.0.1', publicHost: '127.0.0.1', sipPort: 0,
      rtpPortPool: new RtpPortPool({ range: [43000, 43000] }),
      lineFactory({ rtpPort }) {
        const line = new EventEmitter(); line.rtpPort = rtpPort;
        line.start = () => new Promise((resolve, reject) => { ready = resolve; failed = reject; });
        line.stop = async () => { stops++; };
        return line;
      }
    });
    server.sendSip = (_, response) => responses.push(response);
    const task = server.handleInvite(invite(), { address: '192.0.2.1', port: 5060 });
    const retransmit = server.handleInvite(invite(), { address: '192.0.2.1', port: 5060 });
    assert.ok(!responses.some(response => response.startsWith('SIP/2.0 200')));
    if (success) ready(); else failed(new Error('bind failure'));
    await Promise.all([task, retransmit]);
    assert.equal(server.metrics[success ? 'invitesAccepted' : 'invitesRejected'], 1);
    assert.ok(responses.some(response => response.startsWith(`SIP/2.0 ${success ? 200 : 503}`)));
    if (success) server.sessions.terminate('bind-test');
    await Promise.resolve();
    assert.equal(stops, 1);
    assert.equal(server.sessions.activeCount, 0);
    assert.equal(server.sessions.rtpPortPool.available, 1);
  });
}
