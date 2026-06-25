import http from 'node:http';

export class OperatorHttpServer {
  constructor({ host = '127.0.0.1', port = 8080, diagnostics, freepbx }) {
    this.host = host;
    this.port = port;
    this.diagnostics = diagnostics;
    this.freepbx = freepbx;
    this.server = http.createServer((request, response) => {
      this.handleRequest(request, response);
    });
  }

  start() {
    return new Promise((resolve) => {
      this.server.listen(this.port, this.host, resolve);
    });
  }

  stop() {
    return new Promise((resolve, reject) => {
      this.server.close((error) => {
        if (error) {
          reject(error);
          return;
        }
        resolve();
      });
    });
  }

  handleRequest(request, response) {
    if (request.method !== 'GET') {
      sendText(response, 405, 'method not allowed\n');
      return;
    }

    const url = new URL(request.url, `http://${request.headers.host ?? 'localhost'}`);
    if (url.pathname === '/healthz') {
      sendJson(response, 200, buildHealth(this.diagnostics()));
      return;
    }

    if (url.pathname === '/metrics') {
      sendText(response, 200, renderMetrics(this.diagnostics()), 'text/plain; version=0.0.4');
      return;
    }

    if (url.pathname === '/freepbx/pjsip.conf') {
      sendText(response, 200, renderFreePbxPjsip(this.freepbx), 'text/plain');
      return;
    }

    sendText(response, 404, 'not found\n');
  }
}

export function buildHealth(diagnostics) {
  const healthy = diagnostics.sip.listening && diagnostics.rtp.listening && diagnostics.ppp.configuredUsers > 0;

  return {
    status: healthy ? 'ok' : 'degraded',
    checks: {
      sipListening: diagnostics.sip.listening,
      rtpListening: diagnostics.rtp.listening,
      pppUsersConfigured: diagnostics.ppp.configuredUsers > 0,
      sessionCapacityAvailable: diagnostics.sessions.active < diagnostics.sessions.limit
    },
    sessions: diagnostics.sessions,
    ppp: diagnostics.ppp,
    media: diagnostics.media
  };
}

export function renderMetrics(diagnostics) {
  const lines = [
    '# HELP sipfax_up SIPfax process health, 1 when SIP and RTP sockets are listening.',
    '# TYPE sipfax_up gauge',
    `sipfax_up ${diagnostics.sip.listening && diagnostics.rtp.listening ? 1 : 0}`,
    '# HELP sipfax_active_sessions Active SIPfax call sessions.',
    '# TYPE sipfax_active_sessions gauge',
    `sipfax_active_sessions ${diagnostics.sessions.active}`,
    '# HELP sipfax_session_limit Configured concurrent SIPfax call session limit.',
    '# TYPE sipfax_session_limit gauge',
    `sipfax_session_limit ${diagnostics.sessions.limit}`,
    '# HELP sipfax_invites_total SIP INVITE outcomes.',
    '# TYPE sipfax_invites_total counter',
    `sipfax_invites_total{outcome="accepted"} ${diagnostics.metrics.invitesAccepted}`,
    `sipfax_invites_total{outcome="rejected"} ${diagnostics.metrics.invitesRejected}`,
    '# HELP sipfax_rtp_frames_total RTP frames accepted by the modem bridge.',
    '# TYPE sipfax_rtp_frames_total counter',
    `sipfax_rtp_frames_total ${diagnostics.metrics.rtpFramesAccepted}`,
    '# HELP sipfax_rtp_dropped_total RTP packets dropped before modem handoff.',
    '# TYPE sipfax_rtp_dropped_total counter',
    `sipfax_rtp_dropped_total ${diagnostics.metrics.rtpFramesDropped}`,
    '# HELP sipfax_ppp_configured_users PPP users configured for authentication.',
    '# TYPE sipfax_ppp_configured_users gauge',
    `sipfax_ppp_configured_users ${diagnostics.ppp.configuredUsers}`,
    '# HELP sipfax_ppp_active_leases Active PPP address leases.',
    '# TYPE sipfax_ppp_active_leases gauge',
    `sipfax_ppp_active_leases ${diagnostics.ppp.addressPool.activeLeases}`
  ];

  return `${lines.join('\n')}\n`;
}

export function renderFreePbxPjsip({
  name = 'sipfax',
  serverHost,
  sipPort,
  extension = 'faxmodem',
  codecs = ['ulaw', 'alaw']
}) {
  return [
    `; SIPfax trunk for FreePBX/Asterisk PJSIP. Route extension ${extension} to this endpoint.`,
    `; Keep SIPfax outside the Asterisk media path: no T.38, transcoding, recording, VAD, or conferencing.`,
    `[${name}]`,
    'type=endpoint',
    'transport=0.0.0.0-udp',
    'context=from-internal',
    'disallow=all',
    `allow=${codecs.join(',')}`,
    'direct_media=no',
    't38_udptl=no',
    'rtp_symmetric=yes',
    'force_rport=yes',
    'rewrite_contact=yes',
    `aors=${name}`,
    '',
    `[${name}]`,
    'type=aor',
    `contact=sip:${serverHost}:${sipPort}`,
    'max_contacts=1',
    ''
  ].join('\n');
}

function sendJson(response, statusCode, body) {
  const serialized = JSON.stringify(body, null, 2);
  response.writeHead(statusCode, {
    'Content-Type': 'application/json',
    'Cache-Control': 'no-store'
  });
  response.end(`${serialized}\n`);
}

function sendText(response, statusCode, body, contentType = 'text/plain') {
  response.writeHead(statusCode, {
    'Content-Type': contentType,
    'Cache-Control': 'no-store'
  });
  response.end(body);
}
