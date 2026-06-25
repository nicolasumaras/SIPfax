import { G711_CODECS } from './media.js';
import { PppSessionController } from './ppp.js';
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
    this.ppp = null;
  }

  markEstablished(ppp) {
    this.state = 'established';
    this.ppp = ppp;
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
  constructor({ publicHost, localRtpPort, ppp = new PppSessionController() }) {
    this.publicHost = publicHost;
    this.localRtpPort = localRtpPort;
    this.ppp = ppp;
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

    this.activeSession.markEstablished(this.ppp.begin(callId));
    return true;
  }

  authenticatePpp(callId, credentials) {
    if (this.activeSession?.callId !== callId) {
      return { authenticated: false, reason: 'unknown-session' };
    }

    const result = this.ppp.authenticate(callId, credentials);
    if (result.authenticated) {
      this.activeSession.ppp = result.session;
    }

    return result;
  }

  diagnostics() {
    return {
      active: this.activeSession ? 1 : 0,
      limit: 1,
      activeCallId: this.activeSession?.callId ?? null,
      activeSessionState: this.activeSession?.state ?? null,
      ppp: this.ppp.diagnostics()
    };
  }

  terminate(callId) {
    if (this.activeSession?.callId !== callId) {
      return false;
    }

    this.activeSession.markTerminated();
    this.ppp.terminate(callId);
    this.activeSession = null;
    return true;
  }
}

function createSipTag() {
  return Math.random().toString(16).slice(2, 10);
}
