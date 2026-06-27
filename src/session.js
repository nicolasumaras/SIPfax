import { Line } from './line.js';
import { G711_CODECS, RtpPortPool } from './media.js';
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
    this.startedAt = new Date();
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

/**
 * Tracks up to `maxSessions` concurrent calls, keyed by Call-ID. Each call owns
 * a Line (its own RTP port + modem process); PPP lifecycle is delegated to the
 * shared PppSessionController (already keyed by callId). The cap is mutable so
 * the admin UI can raise/lower it live without dropping active calls.
 */
export class MultiSessionManager {
  constructor({
    publicHost,
    rtpHost = '0.0.0.0',
    rtpPortPool = new RtpPortPool(),
    modemFactory = null,
    ppp = new PppSessionController(),
    maxSessions = 1,
    lineFactory = (options) => new Line(options)
  } = {}) {
    this.publicHost = publicHost;
    this.rtpHost = rtpHost;
    this.rtpPortPool = rtpPortPool;
    this.modemFactory = modemFactory;
    this.ppp = ppp;
    this.maxSessions = maxSessions;
    this.lineFactory = lineFactory;
    this.sessions = new Map(); // callId -> { session, line }
  }

  get activeCount() {
    return this.sessions.size;
  }

  setMaxSessions(n) {
    const value = Number.parseInt(n, 10);
    if (Number.isInteger(value) && value >= 1) {
      this.maxSessions = value;
    }
    return this.maxSessions;
  }

  canAcceptInvite() {
    return this.sessions.size < this.maxSessions;
  }

  startFromInvite(invite) {
    const existing = this.sessions.get(invite.callId);
    if (existing) {
      return { accepted: true, session: existing.session, retransmit: true };
    }

    if (this.sessions.size >= this.maxSessions) {
      return { accepted: false, statusCode: 486, reason: 'Busy Here' };
    }

    const offer = parseSdpOffer(invite.body);
    const codec = offer.codecs.find((candidate) => G711_CODECS.has(candidate.payloadType));
    if (!codec) {
      return { accepted: false, statusCode: 488, reason: 'Not Acceptable Here' };
    }
    const supportedCodec = G711_CODECS.get(codec.payloadType);

    const rtpPort = this.rtpPortPool?.allocate?.() ?? null;
    if (rtpPort === null) {
      return { accepted: false, statusCode: 486, reason: 'Busy Here' };
    }

    const modem = this.modemFactory ? this.modemFactory(invite.callId) : null;
    const line = this.lineFactory({
      callId: invite.callId,
      codec: supportedCodec,
      rtpHost: this.rtpHost,
      rtpPort,
      modem
    });
    line.on('pty-opened', ({ callId, slavePath }) => this.openPty(callId, { slavePath }));
    line.on('pty-closed', ({ callId }) => this.closePty(callId));
    line.on('backend-log', ({ callId, line: msg }) => console.log(`modem[${callId}] ${String(msg).trim()}`));
    line.on('backend-error', ({ callId, error }) => console.error(`modem[${callId}] error: ${error?.message ?? error}`));
    // RTP only flows after ACK, so this async bind completes well before media.
    Promise.resolve(line.start()).catch((error) =>
      console.error(`line ${invite.callId} rtp bind failed: ${error.message}`)
    );

    const session = new CallSession({
      callId: invite.callId,
      fromTag: invite.fromTag,
      toTag: createSipTag(),
      invite,
      codec: supportedCodec,
      localRtpPort: rtpPort,
      publicHost: this.publicHost
    });
    this.sessions.set(invite.callId, { session, line });
    return { accepted: true, session };
  }

  acknowledge(callId) {
    const entry = this.sessions.get(callId);
    if (!entry) {
      return false;
    }
    entry.session.markEstablished(this.ppp.begin(callId));
    return true;
  }

  authenticatePpp(callId, credentials) {
    const entry = this.sessions.get(callId);
    if (!entry) {
      return { authenticated: false, reason: 'unknown-session' };
    }
    const result = this.ppp.authenticate(callId, credentials);
    if (result.authenticated) {
      entry.session.ppp = result.session;
    }
    return result;
  }

  openPty(callId, { slavePath }) {
    const entry = this.sessions.get(callId);
    if (!entry || !slavePath) {
      return false;
    }
    const started = this.ppp.startPppd(callId, { slavePath });
    if (started) {
      entry.session.ppp = this.ppp.snapshot(callId);
    }
    return started;
  }

  closePty(callId) {
    const entry = this.sessions.get(callId);
    if (!entry) {
      return false;
    }
    return this.ppp.stopPppd(callId);
  }

  terminate(callId) {
    const entry = this.sessions.get(callId);
    if (!entry) {
      return false;
    }
    entry.session.markTerminated();
    Promise.resolve(entry.line.stop()).catch(() => {});
    if (entry.line.rtpPort != null) {
      this.rtpPortPool?.release?.(entry.line.rtpPort);
    }
    this.ppp.terminate(callId);
    this.sessions.delete(callId);
    return true;
  }

  diagnostics() {
    return {
      active: this.sessions.size,
      limit: this.maxSessions,
      capacity: this.rtpPortPool?.capacity ?? this.maxSessions,
      sessions: [...this.sessions.values()].map(({ session, line }) => {
        const ppp = session.ppp ?? {};
        return {
          callId: session.callId,
          state: session.state,
          username: ppp.username ?? null,
          clientAddress: ppp.lease?.clientAddress ?? ppp.clientAddress ?? null,
          interfaceName: ppp.interfaceName ?? null,
          durationSeconds: session.startedAt
            ? Math.floor((Date.now() - session.startedAt.getTime()) / 1000)
            : null,
          ...line.diagnostics()
        };
      })
    };
  }
}

function createSipTag() {
  return Math.random().toString(16).slice(2, 10);
}
