import dgram from 'node:dgram';
import { ModemBridge, RtpEndpoint } from './media.js';
import { SingleSessionManager } from './session.js';
import { buildResponse, parseSipMessage } from './sip.js';

export class SipFaxServer {
  constructor({ host, publicHost, sipPort, rtpPort, ppp, modem = null }) {
    this.host = host;
    this.publicHost = publicHost;
    this.sipPort = sipPort;
    this.rtpPort = rtpPort;
    this.sipSocket = dgram.createSocket('udp4');
    this.rtpEndpoint = new RtpEndpoint({ host, port: rtpPort });
    this.modem = modem;
    this.modemBridge = new ModemBridge({ modem });
    this.sessions = new SingleSessionManager({ publicHost, localRtpPort: rtpPort, ppp });
    this.startedAt = new Date();
    this.metrics = {
      invitesAccepted: 0,
      invitesRejected: 0,
      rtpFramesAccepted: 0,
      rtpFramesDropped: 0
    };
    this.listening = {
      sip: false,
      rtp: false
    };

    this.rtpEndpoint.on('frame', (frame) => {
      this.metrics.rtpFramesAccepted += 1;
      this.modemBridge.acceptFrame(frame);
    });
    this.modemBridge.on('outbound-audio', (audio) => {
      this.rtpEndpoint.sendPayload(audio.payload, {
        payloadType: audio.payloadType,
        timestampIncrement: audio.timestampIncrement,
        marker: audio.marker
      });
    });
    this.rtpEndpoint.on('dropped', () => {
      this.metrics.rtpFramesDropped += 1;
    });
    this.sipSocket.on('message', (message, remote) => {
      this.handleSipDatagram(message.toString('utf8'), remote);
    });
  }

  async start() {
    await this.rtpEndpoint.start();
    this.listening.rtp = true;
    await new Promise((resolve) => {
      this.sipSocket.bind(this.sipPort, this.host, resolve);
    });
    this.listening.sip = true;
  }

  async stop() {
    if (this.modem?.stop) {
      this.modem.stop();
    }

    await Promise.all([
      new Promise((resolve, reject) => {
        this.sipSocket.close((error) => {
          if (error) {
            reject(error);
            return;
          }
          resolve();
        });
      }),
      this.rtpEndpoint.stop()
    ]);
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
      this.rtpEndpoint.setSessionCodec(null);
      this.modemBridge.setSessionCodec(null);
      if (this.modem?.stop) {
        this.modem.stop();
      }
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

    this.metrics.invitesAccepted += 1;
    const { session } = result;
    this.rtpEndpoint.setSessionCodec(session.codec);
    this.modemBridge.setSessionCodec(session.codec);

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
    return {
      startedAt: this.startedAt.toISOString(),
      uptimeSeconds: Math.floor((Date.now() - this.startedAt.getTime()) / 1000),
      sip: {
        host: this.host,
        port: this.sipPort,
        publicHost: this.publicHost,
        listening: this.listening.sip
      },
      rtp: {
        host: this.host,
        port: this.rtpPort,
        listening: this.listening.rtp
      },
      sessions: this.sessions.diagnostics(),
      ppp: this.sessions.ppp.diagnostics(),
      media: this.modemBridge.diagnostics(),
      metrics: { ...this.metrics }
    };
  }
}
