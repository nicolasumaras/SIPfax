import { spawn } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const DEFAULT_DNS_SERVERS = ['1.1.1.1', '9.9.9.9'];

export function renderChapSecrets(credentials, path) {
  const lines = credentials.chapSecrets().map(({ username, password }) => {
    return `${quotePppSecret(username)} * ${quotePppSecret(password)} *`;
  });
  writeFileSync(path, `${lines.join('\n')}\n`, { mode: 0o600 });
  return path;
}

export function buildPppdArgs({
  slavePath,
  lease,
  dnsServers = DEFAULT_DNS_SERVERS,
  authProtocol = 'chap',
  secretsPath,
  notifyScript = null,
  mtu = 1500,
  lcpEchoInterval = 30,
  lcpEchoFailure = 4,
  lcpRestart = 5,
  lcpMaxConfigure = 20,
  ipcpRestart = 5,
  ipcpMaxConfigure = 20,
  connectDelayMs = 1000,
  callId = null
}) {
  const normalizedAuth = authProtocol === 'pap' ? 'pap' : 'chap';
  const args = [
    slavePath,
    'nodetach',
    'nodefaultroute',
    'noccp',
    `require-${normalizedAuth}`,
    `${lease.localAddress}:${lease.clientAddress}`,
    'mtu',
    String(mtu),
    'lcp-echo-interval',
    String(lcpEchoInterval),
    'lcp-echo-failure',
    String(lcpEchoFailure),
    'lcp-restart',
    String(lcpRestart),
    'lcp-max-configure',
    String(lcpMaxConfigure),
    'ipcp-restart',
    String(ipcpRestart),
    'ipcp-max-configure',
    String(ipcpMaxConfigure),
    'connect-delay',
    String(connectDelayMs)
  ];

  for (const dnsServer of dnsServers) {
    args.push('ms-dns', dnsServer);
  }

  if (secretsPath) {
    args.push(`${normalizedAuth}-secrets`, secretsPath);
  }

  if (notifyScript) {
    args.push('ip-up-script', notifyScript, 'ip-down-script', notifyScript);
  }

  if (callId) {
    args.push('ipparam', callId);
  }

  return args;
}

export class PppdSupervisor extends EventEmitter {
  constructor({
    command = 'pppd',
    authProtocol = 'chap',
    dnsServers = DEFAULT_DNS_SERVERS,
    notifyScript = null,
    leaseDir = '/run/sipfax/ppp-leases',
    secretsDir = '/etc/ppp',
    tempDir = tmpdir(),
    spawnProcess = spawn,
    cleanup = rmSync
  } = {}) {
    super();
    this.command = command;
    this.authProtocol = authProtocol;
    this.dnsServers = [...dnsServers];
    this.notifyScript = notifyScript;
    this.leaseDir = leaseDir;
    this.secretsDir = secretsDir;
    this.tempDir = tempDir;
    this.spawnProcess = spawnProcess;
    this.cleanup = cleanup;
    this.sessions = new Map();
  }

  start({ callId, slavePath, lease, dnsServers = this.dnsServers, credentials, egressDescriptor = null, onEvent = null }) {
    this.stop(callId);

    const sessionDir = mkdtempSync(join(this.tempDir, `sipfax-pppd-${sanitizePathPart(callId)}-`));
    const egressDescriptorPath = egressDescriptor
      ? this.writeEgressDescriptor(callId, egressDescriptor)
      : null;
    const session = {
      callId,
      slavePath,
      lease: { ...lease },
      dnsServers: [...dnsServers],
      sessionDir,
      egressDescriptorPath,
      secretsPath: null,
      process: null,
      startedAt: null,
      endedAt: null,
      state: 'starting',
      interfaceName: null,
      sessionDurationSeconds: null,
      lastEventAt: null,
      lastError: null
    };

    const args = buildPppdArgs({
      slavePath,
      lease,
      dnsServers,
      authProtocol: this.authProtocol,
      notifyScript: this.notifyScript,
      callId
    });

    // pppd has no command-line option to select a secrets file; it always
    // reads /etc/ppp/{chap,pap}-secrets. Render the per-call credentials there
    // before launch (single active call) and remove the file on teardown.
    // secretsDir defaults to /etc/ppp; tests inject a writable temp dir.
    const secretsFile = join(
      this.secretsDir,
      this.authProtocol === 'pap' ? 'pap-secrets' : 'chap-secrets'
    );
    renderChapSecrets(credentials, secretsFile);
    const child = this.spawnProcess(this.command, args, {
      stdio: ['ignore', 'pipe', 'pipe']
    });
    session.process = child;
    session.startedAt = new Date();
    session.secretsPath = secretsFile;
    session.args = args;

    child.stdout?.on('data', (chunk) => {
      this.acceptNotifyChunk(callId, chunk);
    });
    child.stderr?.on('data', (chunk) => {
      const text = chunk.toString('utf8').trim();
      if (text) {
        session.lastError = text;
        this.emit('pppd-log', { callId, line: text });
      }
    });
    child.on('error', (error) => {
      session.lastError = error.message;
      this.acceptEvent(callId, { state: 'failed', error: error.message });
    });
    child.on('exit', (code, signal) => {
      session.endedAt = new Date();
      session.sessionDurationSeconds = Math.max(0, Math.floor((session.endedAt.getTime() - session.startedAt.getTime()) / 1000));
      this.acceptEvent(callId, { state: 'closed', code, signal });
      this.removeSessionFiles(session);
      this.sessions.delete(callId);
    });

    this.sessions.set(callId, session);
    this.acceptEvent(callId, {
      state: 'starting',
      localAddress: lease.localAddress,
      clientAddress: lease.clientAddress,
      dnsServers,
      pid: child.pid,
      secretsPath: session.secretsPath
    });
    onEvent?.(this.snapshot(callId));
    session.onEvent = onEvent;
    return this.snapshot(callId);
  }

  stop(callId) {
    const session = this.sessions.get(callId);
    if (!session) {
      return false;
    }

    if (session.process && !session.process.killed) {
      session.process.kill('SIGTERM');
    }
    this.removeSessionFiles(session);
    this.sessions.delete(callId);
    return true;
  }

  acceptNotifyChunk(callId, chunk) {
    const session = this.sessions.get(callId);
    if (!session) {
      return;
    }

    session.notifyBuffer = `${session.notifyBuffer ?? ''}${chunk.toString('utf8')}`;
    while (true) {
      const newlineIndex = session.notifyBuffer.indexOf('\n');
      if (newlineIndex < 0) {
        return;
      }

      const line = session.notifyBuffer.slice(0, newlineIndex).trim();
      session.notifyBuffer = session.notifyBuffer.slice(newlineIndex + 1);
      if (!line) {
        continue;
      }

      try {
        this.acceptEvent(callId, JSON.parse(line));
      } catch (error) {
        session.lastError = `invalid pppd notify JSON: ${error.message}`;
        this.emit('pppd-error', { callId, error: session.lastError });
      }
    }
  }

  acceptEvent(callId, event) {
    const session = this.sessions.get(callId);
    if (!session) {
      return;
    }

    const normalized = normalizeNotifyEvent(event);
    session.state = normalized.state ?? session.state;
    session.interfaceName = normalized.interfaceName ?? session.interfaceName;
    session.localAddress = normalized.localAddress ?? session.lease.localAddress;
    session.clientAddress = normalized.clientAddress ?? session.lease.clientAddress;
    session.dnsServers = normalized.dnsServers ?? session.dnsServers;
    session.lastEventAt = new Date().toISOString();

    if (session.startedAt) {
      session.sessionDurationSeconds = Math.max(0, Math.floor((Date.now() - session.startedAt.getTime()) / 1000));
    }

    const snapshot = this.snapshot(callId);
    session.onEvent?.(snapshot);
    this.emit('pppd-event', snapshot);
  }

  snapshot(callId) {
    const session = this.sessions.get(callId);
    if (!session) {
      return null;
    }

    return {
      callId,
      state: session.state,
      pid: session.process?.pid ?? null,
      localAddress: session.localAddress ?? session.lease.localAddress,
      clientAddress: session.clientAddress ?? session.lease.clientAddress,
      dnsServers: [...session.dnsServers],
      interfaceName: session.interfaceName,
      sessionDurationSeconds: session.sessionDurationSeconds,
      lastEventAt: session.lastEventAt,
      lastError: session.lastError,
      egressDescriptorPath: session.egressDescriptorPath
    };
  }

  diagnostics() {
    return {
      command: this.command,
      authProtocol: this.authProtocol,
      notifyScript: this.notifyScript,
      leaseDir: this.leaseDir,
      activeSessions: this.sessions.size,
      sessions: [...this.sessions.keys()].map((callId) => this.snapshot(callId))
    };
  }

  removeSessionFiles(session) {
    try {
      this.cleanup(session.sessionDir, { recursive: true, force: true });
    } catch (error) {
      session.lastError = error.message;
    }
    if (session.secretsPath) {
      // The secrets file lives in root-owned /etc/ppp; we own the file but
      // not the directory, so clear it in place rather than unlinking it.
      try {
        writeFileSync(session.secretsPath, '', { mode: 0o600 });
      } catch (error) {
        session.lastError = error.message;
      }
    }
  }

  writeEgressDescriptor(callId, descriptor) {
    mkdirSync(this.leaseDir, { recursive: true, mode: 0o750 });
    const descriptorPath = join(this.leaseDir, `${sanitizePathPart(callId)}.json`);
    writeFileSync(descriptorPath, `${JSON.stringify(descriptor, null, 2)}\n`, { mode: 0o640 });
    return descriptorPath;
  }
}

function normalizeNotifyEvent(event) {
  const rawState = event.state ?? event.event ?? event.lastEvent;
  const state = rawState === 'ip-up' || rawState === 'IPCP-open' || rawState === 'ipcp-open'
    ? 'ipcp-open'
    : rawState === 'ip-down' || rawState === 'IPCP-close' || rawState === 'ipcp-close'
      ? 'ipcp-closed'
      : rawState;

  return {
    state,
    localAddress: event.localAddress ?? event.local ?? event.ipLocal,
    clientAddress: event.clientAddress ?? event.remote ?? event.ipRemote,
    dnsServers: event.dnsServers,
    interfaceName: event.interfaceName ?? event.ifname ?? event.interface,
    sessionDurationSeconds: event.sessionDurationSeconds
  };
}

function quotePppSecret(value) {
  return `"${String(value).replaceAll('\\', '\\\\').replaceAll('"', '\\"')}"`;
}

function sanitizePathPart(value) {
  return String(value).replace(/[^a-zA-Z0-9_.-]/g, '_');
}
