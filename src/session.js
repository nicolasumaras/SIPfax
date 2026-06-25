import { G711_CODECS } from './media.js';
import { buildSdpAnswer, parseSdpOffer } from './sdp.js';

export class CallSession {
  constructor({ callId, fromTag, toTag, invite, codec, localRtpPort, publicHost }) {
    this.callId = callId;
    this.fromTag = fromTag;
    this.toTag = toTag;
    this.invite = invite;
    this.codec = codec;
    this.localRtpPort = localRtpPort;
    this.publicHost = publicHost;
    this.state = 'ringing';
  }

  markEstablished() {
    this.state = 'established';
  }

  markTerminated() {
    this.state = 'terminated';
  }

  sdpAnswer() {
    return buildSdpAnswer({
      host: this.publicHost,
      rtpPort: this.localRtpPort,
      codec: this.codec
    });
  }
}

export class SingleSessionManager {
  constructor({ publicHost, localRtpPort }) {
    this.publicHost = publicHost;
    this.localRtpPort = localRtpPort;
    this.activeSession = null;
  }

  canAcceptInvite() {
    return this.activeSession === null;
  }

  startFromInvite(invite) {
    if (this.activeSession) {
      return { accepted: false, statusCode: 486, reason: 'Busy Here' };
    }

    const offer = parseSdpOffer(invite.body);
    const codec = offer.codecs.find((candidate) => G711_CODECS.has(candidate.payloadType));

    if (!codec) {
      return { accepted: false, statusCode: 488, reason: 'Not Acceptable Here' };
    }

    const supportedCodec = G711_CODECS.get(codec.payloadType);
    const session = new CallSession({
      callId: invite.callId,
      fromTag: invite.fromTag,
      toTag: createSipTag(),
      invite,
      codec: supportedCodec,
      localRtpPort: this.localRtpPort,
      publicHost: this.publicHost
    });

    this.activeSession = session;
    return { accepted: true, session };
  }

  acknowledge(callId) {
    if (this.activeSession?.callId !== callId) {
      return false;
    }

    this.activeSession.markEstablished();
    return true;
  }

  terminate(callId) {
    if (this.activeSession?.callId !== callId) {
      return false;
    }

    this.activeSession.markTerminated();
    this.activeSession = null;
    return true;
  }
}

function createSipTag() {
  return Math.random().toString(16).slice(2, 10);
}
