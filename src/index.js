import { SipFaxServer } from './server.js';
import { AddressPool, EgressPolicy, PppCredentialStore, PppSessionController, parseList, parseUsers } from './ppp.js';

const config = {
  host: process.env.SIPFAX_HOST ?? '0.0.0.0',
  publicHost: process.env.SIPFAX_PUBLIC_HOST ?? '127.0.0.1',
  sipPort: Number.parseInt(process.env.SIPFAX_SIP_PORT ?? '5060', 10),
  rtpPort: Number.parseInt(process.env.SIPFAX_RTP_PORT ?? '40000', 10),
  ppp: new PppSessionController({
    credentials: new PppCredentialStore(parseUsers(process.env.SIPFAX_PPP_USERS)),
    addressPool: new AddressPool({
      cidr: process.env.SIPFAX_PPP_POOL ?? '10.64.0.0/24',
      localAddress: process.env.SIPFAX_PPP_LOCAL_ADDRESS
    }),
    dnsServers: parseList(process.env.SIPFAX_PPP_DNS, ['1.1.1.1', '9.9.9.9']),
    egressPolicy: new EgressPolicy({
      clientCidr: process.env.SIPFAX_PPP_POOL ?? '10.64.0.0/24',
      outboundInterface: process.env.SIPFAX_EGRESS_INTERFACE ?? 'wan0',
      allowInternet: process.env.SIPFAX_EGRESS_ENABLED !== 'false',
      allowDns: process.env.SIPFAX_EGRESS_DNS !== 'false',
      allowedDestinations: parseList(process.env.SIPFAX_EGRESS_ALLOW, ['0.0.0.0/0'])
    })
  })
};

const server = new SipFaxServer(config);

await server.start();

console.log(
  `SIPfax listening on udp://${config.host}:${config.sipPort}, RTP udp://${config.host}:${config.rtpPort}`
);
console.log(`PPP users configured: ${config.ppp.diagnostics().configuredUsers}`);

const shutdown = async () => {
  await server.stop();
  process.exit(0);
};

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
