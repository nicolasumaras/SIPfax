import { SipFaxServer } from './server.js';

const config = {
  host: process.env.SIPFAX_HOST ?? '0.0.0.0',
  publicHost: process.env.SIPFAX_PUBLIC_HOST ?? '127.0.0.1',
  sipPort: Number.parseInt(process.env.SIPFAX_SIP_PORT ?? '5060', 10),
  rtpPort: Number.parseInt(process.env.SIPFAX_RTP_PORT ?? '40000', 10)
};

const server = new SipFaxServer(config);

await server.start();

console.log(
  `SIPfax listening on udp://${config.host}:${config.sipPort}, RTP udp://${config.host}:${config.rtpPort}`
);

const shutdown = async () => {
  await server.stop();
  process.exit(0);
};

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
