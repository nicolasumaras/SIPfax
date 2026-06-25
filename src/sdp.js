export function parseSdpOffer(body = '') {
  const codecs = [];
  const lines = body.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  const rtpmap = new Map();
  let mediaPayloadTypes = [];
  let connectionAddress = null;
  let mediaPort = null;

  for (const line of lines) {
    if (line.startsWith('c=IN IP4 ')) {
      connectionAddress = line.slice('c=IN IP4 '.length);
    }

    if (line.startsWith('m=audio ')) {
      const parts = line.split(/\s+/);
      mediaPort = Number.parseInt(parts[1], 10);
      mediaPayloadTypes = parts.slice(3).map((part) => Number.parseInt(part, 10));
    }

    if (line.startsWith('a=rtpmap:')) {
      const [payload, encoding] = line.slice('a=rtpmap:'.length).split(/\s+/, 2);
      const [name, clockRate] = encoding.split('/');
      rtpmap.set(Number.parseInt(payload, 10), {
        name: name.toUpperCase(),
        clockRate: Number.parseInt(clockRate, 10)
      });
    }
  }

  for (const payloadType of mediaPayloadTypes) {
    const mapped = rtpmap.get(payloadType);
    if (mapped) {
      codecs.push({ payloadType, ...mapped });
      continue;
    }

    if (payloadType === 0) {
      codecs.push({ payloadType, name: 'PCMU', clockRate: 8000 });
    }

    if (payloadType === 8) {
      codecs.push({ payloadType, name: 'PCMA', clockRate: 8000 });
    }
  }

  return { codecs, connectionAddress, mediaPort };
}

export function buildSdpAnswer({ host, rtpPort, codec }) {
  return [
    'v=0',
    `o=sipfax 0 0 IN IP4 ${host}`,
    's=SIPfax',
    `c=IN IP4 ${host}`,
    't=0 0',
    `m=audio ${rtpPort} RTP/AVP ${codec.payloadType}`,
    `a=rtpmap:${codec.payloadType} ${codec.name}/${codec.clockRate}`,
    'a=sendrecv',
    'a=ptime:20'
  ].join('\r\n') + '\r\n';
}
