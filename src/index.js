import { SipFaxServer } from './server.js';
import { OperatorHttpServer } from './operator.js';
import { ExternalModemProcessBackend, RtpPortPool } from './media.js';
import { AddressPool, EgressPolicy, PppCredentialStore, PppSessionController, parseList } from './ppp.js';
import { PppdSupervisor } from './pppd-supervisor.js';
import { SipfaxConfig } from './config.js';

export const DEFAULT_SOFTMODEM_BINARY = '/opt/sipfax/bin/sipfax-softmodem';
export const DEFAULT_SLMODEM_BRIDGE = '/opt/sipfax/bin/sipfax-slmodem-bridge';

const { config, seeded } = SipfaxConfig.load();
if (seeded) {
  try {
    config.save();
  } catch (error) {
    console.warn(`could not persist initial config to ${config.path}: ${error.message}`);
  }
}

const operatorHost = process.env.SIPFAX_OPERATOR_HOST ?? '127.0.0.1';
const operatorPort = Number.parseInt(process.env.SIPFAX_OPERATOR_PORT ?? '8080', 10);

function sanitizeId(value) {
  return String(value).replace(/[^a-zA-Z0-9_.-]/g, '_').slice(0, 64);
}

// One fresh modem backend per call. Reads config.modem at call time so engine /
// modulation changes apply to subsequent calls. Each call gets a unique tty link.
function createModemFactory() {
  const softmodem = process.env.SIPFAX_SOFTMODEM_BINARY ?? DEFAULT_SOFTMODEM_BINARY;
  const bridge = process.env.SIPFAX_SLMODEM_BRIDGE ?? DEFAULT_SLMODEM_BRIDGE;
  const args = parseList(process.env.SIPFAX_MODEM_ARGS, []);
  return (callId) => {
    const modem = config.modem;
    const command = modem.command ?? (modem.engine === 'slmodem' ? bridge : softmodem);
    const env = {};
    if (modem.modulation) env.SIPFAX_MODEM_MODULATION = modem.modulation;
    if (modem.slmodemd) env.SIPFAX_SLMODEMD = modem.slmodemd;
    if (modem.engine === 'slmodem') {
      const tty = `/run/sipfax/slmodem-${sanitizeId(callId)}.tty`;
      env.SIPFAX_MODEM_TTY = tty;
      env.SIPFAX_TTY_LINK = tty;
    }
    return new ExternalModemProcessBackend({ command, args, env });
  };
}

function buildPpp() {
  return new PppSessionController({
    credentials: new PppCredentialStore(config.ppp.users),
    addressPool: new AddressPool({
      cidr: config.ppp.poolCidr,
      localAddress: config.ppp.localAddress ?? undefined
    }),
    dnsServers: config.ppp.dns,
    pppdSupervisor: new PppdSupervisor({
      command: process.env.SIPFAX_PPPD_COMMAND ?? '/usr/sbin/pppd',
      authProtocol: config.ppp.auth,
      dnsServers: config.ppp.dns,
      notifyScript: process.env.SIPFAX_PPP_NOTIFY_SCRIPT || null,
      leaseDir: process.env.SIPFAX_PPP_LEASE_DIR ?? '/run/sipfax/ppp-leases'
    }),
    egressPolicy: new EgressPolicy({
      clientCidr: config.ppp.poolCidr,
      outboundInterface: process.env.SIPFAX_EGRESS_INTERFACE ?? 'wan0',
      operatorUrl: process.env.SIPFAX_OPERATOR_URL ?? `http://${operatorHost}:${operatorPort}`,
      allowInternet: process.env.SIPFAX_EGRESS_ENABLED !== 'false',
      allowDns: process.env.SIPFAX_EGRESS_DNS !== 'false',
      allowedDestinations: parseList(process.env.SIPFAX_EGRESS_ALLOW, ['0.0.0.0/0'])
    })
  });
}

const ppp = buildPpp();
const server = new SipFaxServer({
  host: config.sip.host,
  publicHost: config.sip.publicHost,
  sipPort: config.sip.sipPort,
  rtpHost: config.sip.rtpHost,
  rtpPortPool: new RtpPortPool({ range: config.sip.rtpPortRange }),
  modemFactory: createModemFactory(),
  ppp,
  maxSessions: config.maxSessions
});

const operator = new OperatorHttpServer({
  host: operatorHost,
  port: operatorPort,
  config,
  diagnostics: () => server.diagnostics(),
  freepbx: {
    serverHost: config.sip.publicHost,
    sipPort: config.sip.sipPort,
    extension: process.env.SIPFAX_FREEPBX_EXTENSION ?? 'faxmodem'
  }
});

// Hot-apply config changes (cap and users) without dropping active calls.
config.on('change', ({ hot }) => {
  if (hot.includes('maxSessions')) {
    server.sessions.setMaxSessions(config.maxSessions);
  }
  if (hot.includes('ppp.users')) {
    ppp.credentials = new PppCredentialStore(config.ppp.users);
  }
});

await server.start();
await operator.start();

console.log(
  `SIPfax listening on udp://${config.sip.host}:${config.sip.sipPort}, RTP udp://${config.sip.rtpHost}:${config.sip.rtpPortRange[0]}-${config.sip.rtpPortRange[1]}`
);
console.log(`SIPfax operator/admin HTTP on http://${operatorHost}:${operatorPort} (admin ${config.adminConfigured() ? 'enabled' : 'DISABLED — set an admin password'})`);
console.log(`Max concurrent lines: ${config.maxSessions}; PPP users: ${config.ppp.users.length}; modem engine: ${config.modem.engine}`);

const shutdown = async () => {
  await Promise.all([operator.stop(), server.stop()]);
  process.exit(0);
};

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
