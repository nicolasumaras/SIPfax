import dgram from 'node:dgram';
import { MultiSessionManager } from './session.js';
import { buildResponse, parseSipMessage } from './sip.js';

export class SipFaxServer {
  constructor({ host, publicHost, sipPort, rtpHost, rtpPortPool, modemFactory = null, ppp, maxSessions = 1, lineFactory }) {
    this.host = host;
    this.publicHost = publicHost;
    this.sipPort = sipPort;
    this.rtpHost = rtpHost ?? host;
    this.sipSocket = dgram.createSocket('udp4');
    this.sessions = new MultiSessionManager({
      publicHost,
      rtpHost: this.rtpHost,
      rtpPortPool,
      modemFactory,
      ppp,
      maxSessions,
      ...(lineFactory ? { lineFactory } : {})
    });
    this.startedAt = new Date();
    this.metrics = {
      invitesAccepted: 0,
      invitesRejected: 0,
      rtpFramesAccepted: 0,
      rtpFramesDropped: 0
    };
    this.listening = { sip: false, rtp: false };

    this.sipSocket.on('message', (message, remote) => {
      this.handleSipDatagram(message.toString('utf8'), remote);
    });
  }

  async start() {
    await new Promise((resolve) => {
      this.sipSocket.bind(this.sipPort, this.host, resolve);
    });
    this.listening.sip = true;
    this.listening.rtp = true; // per-call RTP sockets are allocated on demand
  }

  async stop() {
    for (const callId of [...this.sessions.sessions.keys()]) {
      this.sessions.terminate(callId);
    }
    await new Promise((resolve, reject) => {
      this.sipSocket.close((error) => (error ? reject(error) : resolve()));
    });
    this.listening.sip = false;
    this.listening.rtp = false;
  }

  handleSipDatagram(raw, remote) {
    const request = parseSipMessage(raw);

    if (request.method === 'INVITE') {
      this.handleInvite(request, remote);
      return;
    }

    if (request.method === 'ACK') {
      this.sessions.acknowledge(request.callId);
      return;
    }

    if (request.method === 'BYE') {
      this.sessions.terminate(request.callId);
      this.sendSip(remote, buildResponse(request, 200, 'OK'));
      return;
    }

    this.sendSip(remote, buildResponse(request, 405, 'Method Not Allowed'));
  }

  handleInvite(request, remote) {
    const result = this.sessions.startFromInvite(request);

    if (!result.accepted) {
      this.metrics.invitesRejected += 1;
      this.sendSip(remote, buildResponse(request, result.statusCode, result.reason));
      return;
    }

    if (!result.retransmit) {
      this.metrics.invitesAccepted += 1;
    }
    const { session } = result;

    this.sendSip(remote, buildResponse(request, 100, 'Trying'));
    this.sendSip(remote, buildResponse(request, 180, 'Ringing', { toTag: session.toTag }));
    this.sendSip(
      remote,
      buildResponse(request, 200, 'OK', {
        toTag: session.toTag,
        body: session.sdpAnswer(),
        headers: { 'Content-Type': 'application/sdp' }
      })
    );
  }

  sendSip(remote, message) {
    this.sipSocket.send(Buffer.from(message), remote.port, remote.address);
  }

  diagnostics() {
    const sessions = this.sessions.diagnostics();
    let rtpAccepted = this.metrics.rtpFramesAccepted;
    let rtpDropped = this.metrics.rtpFramesDropped;
    for (const s of sessions.sessions) {
      rtpAccepted += s.metrics?.rtpFramesAccepted ?? 0;
      rtpDropped += s.metrics?.rtpFramesDropped ?? 0;
    }
    const pool = this.sessions.rtpPortPool;
    return {
      startedAt: this.startedAt.toISOString(),
      uptimeSeconds: Math.floor((Date.now() - this.startedAt.getTime()) / 1000),
      sip: { host: this.host, port: this.sipPort, publicHost: this.publicHost, listening: this.listening.sip },
      rtp: { host: this.rtpHost, range: pool ? [pool.lo, pool.hi] : null, listening: this.listening.rtp },
      sessions,
      ppp: this.sessions.ppp.diagnostics(),
      media: { activeLines: sessions.active },
      metrics: { ...this.metrics, rtpFramesAccepted: rtpAccepted, rtpFramesDropped: rtpDropped }
    };
  }
}
