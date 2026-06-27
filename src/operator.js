import http from 'node:http';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ADMIN_HTML_PATH = join(dirname(fileURLToPath(import.meta.url)), '..', 'public', 'admin.html');

export class OperatorHttpServer {
  constructor({ host = '127.0.0.1', port = 8080, diagnostics, freepbx, config = null }) {
    this.host = host;
    this.port = port;
    this.diagnostics = diagnostics;
    this.freepbx = freepbx;
    this.config = config;
    this.pppEvents = [];
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
      this.server.close((error) => (error ? reject(error) : resolve()));
    });
  }

  handleRequest(request, response) {
    const url = new URL(request.url, `http://${request.headers.host ?? 'localhost'}`);

    if (request.method === 'POST' && url.pathname === '/ppp/events') {
      this.acceptPppEvent(request, response);
      return;
    }

    // ----- admin API (Basic auth) -------------------------------------
    if (url.pathname === '/admin' || url.pathname.startsWith('/admin/')) {
      if (!this.requireAdmin(request, response)) {
        return;
      }
      this.handleAdmin(request, response, url).catch((error) => {
        sendJson(response, 400, { error: error.message });
      });
      return;
    }

    if (request.method !== 'GET') {
      sendText(response, 405, 'method not allowed\n');
      return;
    }

    if (url.pathname === '/healthz') {
      sendJson(response, 200, buildHealth(this.diagnostics()));
      return;
    }
    if (url.pathname === '/metrics') {
      sendText(response, 200, renderMetrics(this.diagnostics()), 'text/plain; version=0.0.4');
      return;
    }
    if (url.pathname === '/freepbx/pjsip.conf') {
      sendText(response, 200, renderFreePbxPjsip(this.freepbxParams()), 'text/plain');
      return;
    }
    if (url.pathname === '/ppp/events') {
      sendJson(response, 200, { events: this.pppEvents });
      return;
    }

    sendText(response, 404, 'not found\n');
  }

  // ----- admin -------------------------------------------------------
  requireAdmin(request, response) {
    if (!this.config?.adminConfigured()) {
      sendJson(response, 503, { error: 'admin UI not configured: set an admin password' });
      return false;
    }
    const header = request.headers.authorization ?? '';
    const [scheme, encoded] = header.split(' ');
    if (scheme === 'Basic' && encoded) {
      const [user, ...rest] = Buffer.from(encoded, 'base64').toString('utf8').split(':');
      if (this.config.verifyAdmin(user, rest.join(':'))) {
        return true;
      }
    }
    response.writeHead(401, {
      'WWW-Authenticate': 'Basic realm="SIPfax admin", charset="UTF-8"',
      'Content-Type': 'application/json'
    });
    response.end(JSON.stringify({ error: 'authentication required' }));
    return false;
  }

  async handleAdmin(request, response, url) {
    const { method } = request;
    const path = url.pathname;

    if (method === 'GET' && path === '/admin') {
      try {
        sendText(response, 200, readFileSync(ADMIN_HTML_PATH, 'utf8'), 'text/html; charset=utf-8');
      } catch {
        sendText(response, 500, 'admin UI asset missing\n');
      }
      return;
    }
    if (method === 'GET' && path === '/admin/config') {
      sendJson(response, 200, this.config.redacted());
      return;
    }
    if (method === 'GET' && path === '/admin/sessions') {
      sendJson(response, 200, this.diagnostics().sessions);
      return;
    }
    if (method === 'GET' && path === '/admin/freepbx') {
      sendText(response, 200, renderFreePbxPjsip(this.freepbxParams()), 'text/plain');
      return;
    }
    if (method === 'POST' && path === '/admin/users') {
      const body = await readJsonBody(request, 8192);
      const result = this.config.addUser({ username: body.username, password: body.password });
      sendJson(response, 200, { ok: true, ...result });
      return;
    }
    if (method === 'DELETE' && path.startsWith('/admin/users/')) {
      const name = decodeURIComponent(path.slice('/admin/users/'.length));
      const result = this.config.removeUser(name);
      sendJson(response, 200, { ok: true, ...result });
      return;
    }
    if (method === 'PUT' && path === '/admin/cap') {
      const body = await readJsonBody(request, 4096);
      const result = this.config.setMaxSessions(body.maxSessions);
      sendJson(response, 200, { ok: true, ...result });
      return;
    }
    if (method === 'PUT' && path === '/admin/sip') {
      const body = await readJsonBody(request, 8192);
      const result = this.config.setSip(body);
      sendJson(response, 200, { ok: true, ...result });
      return;
    }

    sendText(response, 404, 'not found\n');
  }

  freepbxParams() {
    return {
      ...this.freepbx,
      codecs: this.config?.trunk?.codecs ?? this.freepbx?.codecs,
      maxChannels: this.config?.maxSessions ?? this.freepbx?.maxChannels ?? 1
    };
  }

  acceptPppEvent(request, response) {
    readJsonBody(request, 8192)
      .then((event) => {
        this.pppEvents.push({ ...event, receivedAt: new Date().toISOString() });
        this.pppEvents = this.pppEvents.slice(-20);
        sendJson(response, 202, { accepted: true });
      })
      .catch((error) => {
        sendJson(response, 400, { error: error.message });
      });
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
  codecs = ['ulaw', 'alaw'],
  maxChannels = 1
}) {
  return [
    `; SIPfax PJSIP TRUNK for FreePBX/Asterisk. Route inbound DIDs / extension ${extension} here.`,
    `; Set the trunk "Maximum Channels" to ${maxChannels} (the SIPfax concurrency cap).`,
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
    `max_contacts=${maxChannels}`,
    'qualify_frequency=30',
    ''
  ].join('\n');
}

function sendJson(response, statusCode, body) {
  const serialized = JSON.stringify(body, null, 2);
  response.writeHead(statusCode, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  response.end(`${serialized}\n`);
}

function sendText(response, statusCode, body, contentType = 'text/plain') {
  response.writeHead(statusCode, { 'Content-Type': contentType, 'Cache-Control': 'no-store' });
  response.end(body);
}

function readJsonBody(request, maxBytes) {
  return new Promise((resolve, reject) => {
    let body = '';
    request.setEncoding('utf8');
    request.on('data', (chunk) => {
      body += chunk;
      if (body.length > maxBytes) {
        reject(new Error('request body too large'));
        request.destroy();
      }
    });
    request.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (error) {
        reject(new Error(`invalid JSON: ${error.message}`));
      }
    });
    request.on('error', reject);
  });
}
