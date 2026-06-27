import { EventEmitter } from 'node:events';
import { mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { hashPassword, parseList, parseUsers } from './ppp.js';

export const DEFAULT_CONFIG_FILE = '/etc/sipfax/config.json';

// Keys that cannot be hot-applied (sockets are bound / pools built at startup).
// A change to any of these is persisted but flagged needsRestart for the UI.
const STRUCTURAL_PATHS = new Set([
  'sip.host', 'sip.sipPort', 'sip.publicHost', 'sip.rtpHost', 'sip.rtpPortRange',
  'ppp.poolCidr', 'ppp.localAddress', 'ppp.auth', 'admin.host'
]);

/**
 * Runtime configuration for SIPfax. Backed by a JSON file; seeded from the
 * historical environment variables when the file is absent (full backward
 * compatibility). The admin UI edits this; user/cap changes hot-apply, while
 * structural changes are persisted and flagged needsRestart.
 */
export class SipfaxConfig extends EventEmitter {
  constructor(data, { path = DEFAULT_CONFIG_FILE } = {}) {
    super();
    this.path = path;
    this.data = data;
  }

  static load({ path = process.env.SIPFAX_CONFIG_FILE ?? DEFAULT_CONFIG_FILE, env = process.env } = {}) {
    let data;
    let onDisk = false;
    try {
      data = JSON.parse(readFileSync(path, 'utf8'));
      onDisk = true;
    } catch (error) {
      if (error.code !== 'ENOENT') {
        throw new Error(`failed to read SIPfax config ${path}: ${error.message}`);
      }
      data = seedFromEnv(env);
    }
    return { config: new SipfaxConfig(normalize(data), { path }), seeded: !onDisk };
  }

  // ----- accessors -------------------------------------------------------
  get maxSessions() { return this.data.maxSessions; }
  get ppp() { return this.data.ppp; }
  get sip() { return this.data.sip; }
  get trunk() { return this.data.trunk; }
  get modem() { return this.data.modem; }
  get admin() { return this.data.admin; }

  adminConfigured() { return Boolean(this.data.admin?.passwordHash); }

  /** Verify HTTP Basic credentials against the configured admin account. */
  verifyAdmin(username, password) {
    const a = this.data.admin;
    if (!a?.passwordHash || !username || !password) return false;
    if (username !== (a.username ?? 'admin')) return false;
    return hashPassword(password) === a.passwordHash; // both server-side, equal-length hex
  }

  /** Config view safe to return over the API: plaintext secrets removed. */
  redacted() {
    const d = this.data;
    return {
      maxSessions: d.maxSessions,
      admin: { username: d.admin?.username ?? 'admin', configured: this.adminConfigured() },
      ppp: {
        users: d.ppp.users.map((u) => ({ username: u.username })),
        poolCidr: d.ppp.poolCidr,
        localAddress: d.ppp.localAddress ?? null,
        dns: d.ppp.dns,
        auth: d.ppp.auth
      },
      sip: { ...d.sip },
      trunk: { ...d.trunk },
      modem: { ...d.modem }
    };
  }

  // ----- mutations (persist + emit) -------------------------------------
  setMaxSessions(n) {
    const value = Number.parseInt(n, 10);
    if (!Number.isInteger(value) || value < 1) throw new Error('cap must be an integer >= 1');
    this.data.maxSessions = value;
    return this.#commit([{ path: 'maxSessions', value }]);
  }

  addUser({ username, password }) {
    if (!username || !password) throw new Error('username and password are required');
    if (/[:,\s]/.test(username)) throw new Error('username may not contain ":", "," or whitespace');
    const users = this.data.ppp.users.filter((u) => u.username !== username);
    users.push({ username, password });
    this.data.ppp.users = users;
    return this.#commit([{ path: 'ppp.users', value: this.data.ppp.users }]);
  }

  removeUser(username) {
    const before = this.data.ppp.users.length;
    this.data.ppp.users = this.data.ppp.users.filter((u) => u.username !== username);
    if (this.data.ppp.users.length === before) throw new Error(`no such user: ${username}`);
    return this.#commit([{ path: 'ppp.users', value: this.data.ppp.users }]);
  }

  setSip(partial) {
    const changes = [];
    for (const [k, v] of Object.entries(partial)) {
      if (!(k in this.data.sip)) throw new Error(`unknown sip setting: ${k}`);
      this.data.sip[k] = v;
      changes.push({ path: `sip.${k}`, value: v });
    }
    return this.#commit(changes);
  }

  setAdminPassword(password, username) {
    if (!password) throw new Error('admin password is required');
    this.data.admin = { username: username ?? this.data.admin?.username ?? 'admin', passwordHash: hashPassword(password) };
    return this.#commit([{ path: 'admin.passwordHash', value: '***' }]);
  }

  #commit(changes) {
    const needsRestart = changes.filter((c) => STRUCTURAL_PATHS.has(c.path)).map((c) => c.path);
    const hot = changes.filter((c) => !STRUCTURAL_PATHS.has(c.path)).map((c) => c.path);
    this.save();
    const result = { hot, needsRestart };
    this.emit('change', { ...result, changes });
    return result;
  }

  save() {
    mkdirSync(dirname(this.path), { recursive: true });
    const tmp = join(dirname(this.path), `.config.tmp-${process.pid}`);
    writeFileSync(tmp, `${JSON.stringify(this.data, null, 2)}\n`, { mode: 0o600 });
    renameSync(tmp, this.path);
    return this.path;
  }
}

function seedFromEnv(env) {
  const adminPass = env.SIPFAX_ADMIN_PASSWORD;
  return {
    maxSessions: Number.parseInt(env.SIPFAX_MAX_SESSIONS ?? '1', 10) || 1,
    admin: {
      username: env.SIPFAX_ADMIN_USER ?? 'admin',
      passwordHash: env.SIPFAX_ADMIN_PASSWORD_HASH ?? (adminPass ? hashPassword(adminPass) : null)
    },
    ppp: {
      users: parseUsers(env.SIPFAX_PPP_USERS),
      poolCidr: env.SIPFAX_PPP_POOL ?? '10.64.0.0/24',
      localAddress: env.SIPFAX_PPP_LOCAL_ADDRESS ?? null,
      dns: parseList(env.SIPFAX_PPP_DNS, ['1.1.1.1', '9.9.9.9']),
      auth: env.SIPFAX_PPP_AUTH === 'pap' ? 'pap' : 'chap'
    },
    sip: {
      host: env.SIPFAX_HOST ?? '0.0.0.0',
      publicHost: env.SIPFAX_PUBLIC_HOST ?? '127.0.0.1',
      sipPort: Number.parseInt(env.SIPFAX_SIP_PORT ?? '5060', 10),
      rtpHost: env.SIPFAX_RTP_HOST ?? env.SIPFAX_HOST ?? '0.0.0.0',
      rtpPortRange: parseRange(env.SIPFAX_RTP_PORT_RANGE, env.SIPFAX_RTP_PORT)
    },
    trunk: { codecs: parseList(env.SIPFAX_TRUNK_CODECS, ['ulaw', 'alaw']) },
    modem: {
      engine: (env.SIPFAX_MODEM_ENGINE ?? 'spandsp').toLowerCase(),
      modulation: env.SIPFAX_MODEM_MODULATION ?? 'v34',
      slmodemd: env.SIPFAX_SLMODEMD ?? null,
      command: env.SIPFAX_MODEM_COMMAND ?? null
    }
  };
}

function parseRange(rangeStr, single) {
  if (rangeStr) {
    const [lo, hi] = rangeStr.split('-').map((n) => Number.parseInt(n.trim(), 10));
    if (Number.isInteger(lo) && Number.isInteger(hi) && hi >= lo) return [lo, hi];
  }
  if (single) {
    const p = Number.parseInt(single, 10);
    if (Number.isInteger(p)) return [p, p + 100];
  }
  return [40000, 40100];
}

// Fill any missing keys so the rest of the app can rely on the shape.
function normalize(data) {
  const seeded = seedFromEnv({});
  return {
    maxSessions: data.maxSessions ?? seeded.maxSessions,
    admin: { ...seeded.admin, ...(data.admin ?? {}) },
    ppp: { ...seeded.ppp, ...(data.ppp ?? {}), users: data.ppp?.users ?? [] },
    sip: { ...seeded.sip, ...(data.sip ?? {}) },
    trunk: { ...seeded.trunk, ...(data.trunk ?? {}) },
    modem: { ...seeded.modem, ...(data.modem ?? {}) }
  };
}
