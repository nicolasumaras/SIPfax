import dgram from 'node:dgram';
import { ModemBridge, RtpEndpoint } from './media.js';
import { SingleSessionManager } from './session.js';
import { buildResponse, parseSipMessage } from './sip.js';

export class SipFaxServer {
  constructor({ host, publicHost, sipPort, rtpPort, ppp }) {
    this.host = host;
    this.publicHost = publicHost;
    this.sipPort = sipPort;
    this.rtpPort = rtpPort;
    this.sipSocket = dgram.createSocket('udp4');
    this.rtpEndpoint = new RtpEndpoint({ host, port: rtpPort });
    this.modemBridge = new ModemBridge();
    this.sessions = new SingleSessionManager({ publicHost, localRtpPort: rtpPort, ppp });

    this.rtpEndpoint.on('frame', (frame) => this.modemBridge.acceptFrame(frame));
    this.sipSocket.on('message', (message, remote) => {
      this.handleSipDatagram(message.toString('utf8'), remote);
    });
  }

  async start() {
    await this.rtpEndpoint.start();
    await new Promise((resolve) => {
      this.sipSocket.bind(this.sipPort, this.host, resolve);
    });
  }

  async stop() {
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
      this.sendSip(remote, buildResponse(request, 200, 'OK'));
      return;
    }

    this.sendSip(remote, buildResponse(request, 405, 'Method Not Allowed'));
  }

  handleInvite(request, remote) {
    const result = this.sessions.startFromInvite(request);

    if (!result.accepted) {
      this.sendSip(remote, buildResponse(request, result.statusCode, result.reason));
      return;
    }

    const { session } = result;
    this.rtpEndpoint.setSessionCodec(session.codec);

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
}
