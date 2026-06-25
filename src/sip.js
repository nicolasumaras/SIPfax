export function parseSipMessage(raw) {
  const [head, ...bodyParts] = raw.split(/\r?\n\r?\n/);
  const body = bodyParts.join('\r\n\r\n');
  const lines = head.split(/\r?\n/);
  const startLine = lines.shift();
  const headers = new Map();

  for (const line of lines) {
    const separator = line.indexOf(':');
    if (separator === -1) {
      continue;
    }

    const name = line.slice(0, separator).trim().toLowerCase();
    const value = line.slice(separator + 1).trim();
    const existing = headers.get(name);
    headers.set(name, existing ? `${existing}, ${value}` : value);
  }

  const [method, requestUri] = startLine.split(/\s+/, 2);
  const callId = headers.get('call-id');
  const from = headers.get('from') ?? headers.get('f') ?? '';

  return {
    raw,
    startLine,
    method,
    requestUri,
    headers,
    body,
    callId,
    fromTag: getHeaderParameter(from, 'tag'),
    branch: getHeaderParameter(headers.get('via') ?? headers.get('v') ?? '', 'branch'),
    cseq: headers.get('cseq')
  };
}

export function buildResponse(request, statusCode, reason, { toTag, body = '', headers = {} } = {}) {
  const to = withTag(request.headers.get('to') ?? request.headers.get('t') ?? '<sip:sipfax>', toTag);
  const lines = [
    `SIP/2.0 ${statusCode} ${reason}`,
    `Via: ${request.headers.get('via') ?? request.headers.get('v')}`,
    `From: ${request.headers.get('from') ?? request.headers.get('f')}`,
    `To: ${to}`,
    `Call-ID: ${request.callId}`,
    `CSeq: ${request.cseq}`
  ];

  for (const [name, value] of Object.entries(headers)) {
    lines.push(`${name}: ${value}`);
  }

  lines.push(`Content-Length: ${Buffer.byteLength(body)}`);
  lines.push('');
  lines.push(body);

  return lines.join('\r\n');
}

export function getHeaderParameter(headerValue, parameterName) {
  const pattern = new RegExp(`(?:^|;)\\s*${parameterName}=([^;>]+)`, 'i');
  return pattern.exec(headerValue)?.[1] ?? null;
}

function withTag(headerValue, tag) {
  if (!tag || /;\s*tag=/i.test(headerValue)) {
    return headerValue;
  }

  return `${headerValue};tag=${tag}`;
}
