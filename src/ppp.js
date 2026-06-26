import { createHash, timingSafeEqual } from 'node:crypto';

const DEFAULT_DNS_SERVERS = ['1.1.1.1', '9.9.9.9'];
const DEFAULT_BLOCKED_DESTINATIONS = [
  '0.0.0.0/8',
  '10.0.0.0/8',
  '100.64.0.0/10',
  '127.0.0.0/8',
  '169.254.0.0/16',
  '172.16.0.0/12',
  '192.0.2.0/24',
  '192.168.0.0/16',
  '198.18.0.0/15',
  '198.51.100.0/24',
  '203.0.113.0/24',
  '224.0.0.0/4',
  '240.0.0.0/4'
];

export class PppCredentialStore {
  constructor(users = []) {
    this.users = new Map();
    this.secrets = new Map();

    for (const user of users) {
      this.addUser(user);
    }
  }

  addUser({ username, password, passwordHash }) {
    if (!username) {
      throw new Error('PPP username is required');
    }

    if (!password && !passwordHash) {
      throw new Error(`PPP password or passwordHash is required for ${username}`);
    }

    this.users.set(username, passwordHash ?? hashPassword(password));
    if (password) {
      this.secrets.set(username, password);
    }
  }

  verify({ username, password }) {
    const expectedHash = this.users.get(username);
    if (!expectedHash || !password) {
      return false;
    }

    const actualHash = hashPassword(password);
    return safeEqual(expectedHash, actualHash);
  }

  get size() {
    return this.users.size;
  }

  chapSecrets() {
    return [...this.users.keys()].map((username) => {
      const password = this.secrets.get(username);
      if (!password) {
        throw new Error(`PPP secret for ${username} is not renderable from a passwordHash-only credential`);
      }

      return { username, password };
    });
  }
}

export class AddressPool {
  constructor({ cidr = '10.64.0.0/24', localAddress } = {}) {
    const parsed = parseCidr(cidr);
    if (parsed.prefixLength > 30) {
      throw new Error('PPP address pool must contain at least two usable host addresses');
    }

    this.cidr = cidr;
    this.network = parsed.network;
    this.broadcast = parsed.broadcast;
    this.prefixLength = parsed.prefixLength;
    this.localAddress = localAddress ?? intToIp(this.network + 1);
    this.leases = new Map();
  }

  lease(callId) {
    const existing = this.leases.get(callId);
    if (existing) {
      return existing;
    }

    for (let address = this.network + 2; address < this.broadcast; address += 1) {
      const clientAddress = intToIp(address);
      if (clientAddress === this.localAddress || this.isLeased(clientAddress)) {
        continue;
      }

      const lease = {
        callId,
        localAddress: this.localAddress,
        clientAddress,
        cidr: this.cidr,
        prefixLength: this.prefixLength
      };
      this.leases.set(callId, lease);
      return lease;
    }

    throw new Error(`PPP address pool ${this.cidr} is exhausted`);
  }

  release(callId) {
    this.leases.delete(callId);
  }

  isLeased(clientAddress) {
    for (const lease of this.leases.values()) {
      if (lease.clientAddress === clientAddress) {
        return true;
      }
    }

    return false;
  }
}

export class EgressPolicy {
  constructor({
    clientCidr = '10.64.0.0/24',
    outboundInterface = 'wan0',
    allowInternet = true,
    allowDns = true,
    allowedDestinations = ['0.0.0.0/0'],
    blockedDestinations = DEFAULT_BLOCKED_DESTINATIONS
  } = {}) {
    this.clientCidr = clientCidr;
    this.outboundInterface = outboundInterface;
    this.allowInternet = allowInternet;
    this.allowDns = allowDns;
    this.allowedDestinations = allowedDestinations.map(parseCidr);
    this.blockedDestinations = blockedDestinations.map(parseCidr);
  }

  allows({ destination, protocol = 'tcp', destinationPort }) {
    if (!this.allowInternet) {
      return false;
    }

    if (protocol === 'udp' && destinationPort === 53 && !this.allowDns) {
      return false;
    }

    const destinationInt = ipToInt(destination);
    if (this.blockedDestinations.some((cidr) => cidrContains(cidr, destinationInt))) {
      return false;
    }

    return this.allowedDestinations.some((cidr) => cidrContains(cidr, destinationInt));
  }

  firewallRules() {
    const rules = [
      `sysctl -w net.ipv4.ip_forward=${this.allowInternet ? '1' : '0'}`,
      `iptables -A FORWARD -s ${this.clientCidr} -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT`,
      `iptables -A FORWARD -d ${this.clientCidr} -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT`
    ];

    for (const destination of this.blockedDestinations) {
      rules.push(`iptables -A FORWARD -s ${this.clientCidr} -d ${formatCidr(destination)} -j REJECT`);
    }

    if (this.allowDns) {
      rules.push(`iptables -A FORWARD -s ${this.clientCidr} -p udp --dport 53 -j ACCEPT`);
      rules.push(`iptables -A FORWARD -s ${this.clientCidr} -p tcp --dport 53 -j ACCEPT`);
    }

    if (this.allowInternet) {
      for (const destination of this.allowedDestinations) {
        rules.push(
          `iptables -A FORWARD -s ${this.clientCidr} -d ${formatCidr(destination)} -o ${this.outboundInterface} -j ACCEPT`
        );
      }
      rules.push(`iptables -t nat -A POSTROUTING -s ${this.clientCidr} -o ${this.outboundInterface} -j MASQUERADE`);
    }

    rules.push(`iptables -A FORWARD -s ${this.clientCidr} -j REJECT`);
    return rules;
  }

  diagnostics() {
    return {
      clientCidr: this.clientCidr,
      outboundInterface: this.outboundInterface,
      allowInternet: this.allowInternet,
      allowDns: this.allowDns,
      allowedDestinations: this.allowedDestinations.map(formatCidr),
      blockedDestinations: this.blockedDestinations.map(formatCidr)
    };
  }
}

export class PppSessionController {
  constructor({
    credentials,
    addressPool = new AddressPool(),
    dnsServers = DEFAULT_DNS_SERVERS,
    egressPolicy = new EgressPolicy({ clientCidr: addressPool.cidr }),
    pppdSupervisor = null
  } = {}) {
    this.credentials = credentials ?? new PppCredentialStore();
    this.addressPool = addressPool;
    this.dnsServers = dnsServers;
    this.egressPolicy = egressPolicy;
    this.pppdSupervisor = pppdSupervisor;
    this.sessions = new Map();
  }

  begin(callId) {
    const session = {
      callId,
      state: 'awaiting-auth',
      authenticatedAt: null,
      username: null,
      lease: null,
      dnsServers: [],
      pppd: null,
      egress: this.egressPolicy.diagnostics()
    };

    this.sessions.set(callId, session);
    return this.snapshot(callId);
  }

  authenticate(callId, credentials) {
    const session = this.sessions.get(callId);
    if (!session) {
      return { authenticated: false, reason: 'unknown-session' };
    }

    if (!this.credentials.verify(credentials)) {
      session.state = 'auth-rejected';
      return { authenticated: false, reason: 'invalid-credentials', session: this.snapshot(callId) };
    }

    session.state = 'authenticated';
    session.authenticatedAt = new Date().toISOString();
    session.username = credentials.username;
    session.lease = this.addressPool.lease(callId);
    session.dnsServers = [...this.dnsServers];

    return { authenticated: true, session: this.snapshot(callId) };
  }

  terminate(callId) {
    const session = this.sessions.get(callId);
    if (!session) {
      return false;
    }

    this.stopPppd(callId);
    this.addressPool.release(callId);
    this.sessions.delete(callId);
    return true;
  }

  startPppd(callId, { slavePath }) {
    const session = this.sessions.get(callId);
    if (!session || !this.pppdSupervisor) {
      return false;
    }

    session.lease = session.lease ?? this.addressPool.lease(callId);
    session.dnsServers = [...this.dnsServers];
    session.state = 'pppd-starting';
    session.pppd = this.pppdSupervisor.start({
      callId,
      slavePath,
      lease: session.lease,
      dnsServers: this.dnsServers,
      credentials: this.credentials,
      onEvent: (event) => {
        this.acceptPppdEvent(callId, event);
      }
    });
    return true;
  }

  stopPppd(callId) {
    if (!this.pppdSupervisor) {
      return false;
    }

    return this.pppdSupervisor.stop(callId);
  }

  acceptPppdEvent(callId, event) {
    const session = this.sessions.get(callId);
    if (!session) {
      return;
    }

    if (event.state) {
      session.state = event.state;
    }

    session.pppd = {
      ...(session.pppd ?? {}),
      ...event,
      dnsServers: [...(event.dnsServers ?? session.dnsServers)]
    };
  }

  snapshot(callId) {
    const session = this.sessions.get(callId);
    if (!session) {
      return null;
    }

    return {
      callId: session.callId,
      state: session.state,
      authenticatedAt: session.authenticatedAt,
      username: session.username,
      lease: session.lease ? { ...session.lease } : null,
      dnsServers: [...session.dnsServers],
      pppd: session.pppd ? { ...session.pppd } : null,
      egress: { ...session.egress }
    };
  }

  diagnostics() {
    return {
      configuredUsers: this.credentials.size,
      addressPool: {
        cidr: this.addressPool.cidr,
        localAddress: this.addressPool.localAddress,
        activeLeases: this.addressPool.leases.size
      },
      pppd: this.pppdSupervisor?.diagnostics ? this.pppdSupervisor.diagnostics() : null,
      egress: this.egressPolicy.diagnostics(),
      sessions: [...this.sessions.keys()].map((callId) => this.snapshot(callId))
    };
  }
}

export function parseUsers(value) {
  if (!value) {
    return [];
  }

  return value.split(',').map((entry) => {
    const [username, password] = entry.split(':');
    if (!username || !password) {
      throw new Error('SIPFAX_PPP_USERS entries must be formatted as username:password');
    }

    return { username, password };
  });
}

export function parseList(value, fallback) {
  if (!value) {
    return fallback;
  }

  return value.split(',').map((entry) => entry.trim()).filter(Boolean);
}

function hashPassword(password) {
  return createHash('sha256').update(password).digest('hex');
}

function safeEqual(left, right) {
  const leftBuffer = Buffer.from(left, 'hex');
  const rightBuffer = Buffer.from(right, 'hex');

  if (leftBuffer.length !== rightBuffer.length) {
    return false;
  }

  return timingSafeEqual(leftBuffer, rightBuffer);
}

function parseCidr(cidr) {
  const [address, prefix] = cidr.split('/');
  const prefixLength = Number.parseInt(prefix, 10);
  if (!Number.isInteger(prefixLength) || prefixLength < 0 || prefixLength > 32) {
    throw new Error(`Invalid IPv4 CIDR prefix: ${cidr}`);
  }

  const mask = prefixLength === 0 ? 0 : (0xffffffff << (32 - prefixLength)) >>> 0;
  const addressInt = ipToInt(address);
  const network = (addressInt & mask) >>> 0;
  const broadcast = (network | (~mask >>> 0)) >>> 0;
  return { address, prefixLength, mask, network, broadcast };
}

function cidrContains(cidr, addressInt) {
  return (addressInt & cidr.mask) >>> 0 === cidr.network;
}

function formatCidr(cidr) {
  return `${intToIp(cidr.network)}/${cidr.prefixLength}`;
}

function ipToInt(address) {
  const octets = address.split('.').map((part) => Number.parseInt(part, 10));
  if (octets.length !== 4 || octets.some((octet) => !Number.isInteger(octet) || octet < 0 || octet > 255)) {
    throw new Error(`Invalid IPv4 address: ${address}`);
  }

  return (
    ((octets[0] << 24) >>> 0) +
    ((octets[1] << 16) >>> 0) +
    ((octets[2] << 8) >>> 0) +
    octets[3]
  ) >>> 0;
}

function intToIp(value) {
  return [
    (value >>> 24) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 8) & 0xff,
    value & 0xff
  ].join('.');
}
