import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { RtpPortPool } from '../src/media.js';
import { SipfaxConfig } from '../src/config.js';
import { PppCredentialStore } from '../src/ppp.js';
import { OperatorHttpServer } from '../src/operator.js';
import { MultiSessionManager } from '../src/session.js';
import { parseSipMessage } from '../src/sip.js';

function makeInvite({ callId, payloads = '0' }) {
  const media = `m=audio 5004 RTP/AVP ${payloads}\r\n`;
  const body = `v=0\r\no=- 1 1 IN IP4 192.0.2.1\r\ns=-\r\nc=IN IP4 192.0.2.1\r\nt=0 0\r\n${media}`;
  return `INVITE sip:fax@example SIP/2.0\r\nCall-ID: ${callId}\r\nFrom: <sip:a@x>;tag=ft\r\nTo: <sip:fax@x>\r\nContent-Type: application/sdp\r\nContent-Length: ${body.length}\r\n\r\n${body}`;
}
class FakeLine extends EventEmitter {
  constructor({ callId, rtpPort }) { super(); this.callId = callId; this.rtpPort = rtpPort; }
  start() { return Promise.resolve(); }
  stop() { return Promise.resolve(); }
  diagnostics() { return { rtpPort: this.rtpPort, metrics: {} }; }
}
function tmpConfigPath() {
  return join(mkdtempSync(join(tmpdir(), 'sipfax-cfg-')), 'config.json');
}

// ---------------------------------------------------------------- RTP pool
test('RtpPortPool allocates distinct even ports, exhausts, and releases', () => {
  const pool = new RtpPortPool({ range: [40000, 40004] });
  assert.equal(pool.capacity, 3); // 40000, 40002, 40004
  const a = pool.allocate();
  const b = pool.allocate();
  assert.equal(a % 2, 0);
  assert.notEqual(a, b);
  const c = pool.allocate();
  assert.equal(pool.allocate(), null); // exhausted
  pool.release(b);
  assert.equal(pool.allocate(), b); // reused
  assert.equal(pool.available, 0);
  assert.ok([a, c].every((p) => p >= 40000 && p <= 40004));
});

// ---------------------------------------------------------------- config
test('config seeds from env, redacts secrets, and classifies hot vs structural changes', () => {
  const path = tmpConfigPath();
  const { config, seeded } = SipfaxConfig.load({
    path,
    env: { SIPFAX_PPP_USERS: 'alice:secret1', SIPFAX_MAX_SESSIONS: '4', SIPFAX_ADMIN_PASSWORD: 'admin-pw' }
  });
  assert.equal(seeded, true);
  assert.equal(config.maxSessions, 4);
  assert.equal(config.ppp.users[0].username, 'alice');

  const red = config.redacted();
  assert.equal(red.ppp.users[0].username, 'alice');
  assert.equal('password' in red.ppp.users[0], false); // no plaintext leaked
  assert.equal(red.admin.configured, true);

  const cap = config.setMaxSessions(8);
  assert.deepEqual(cap, { hot: ['maxSessions'], needsRestart: [] });
  const sip = config.setSip({ sipPort: 5070 });
  assert.deepEqual(sip.needsRestart, ['sip.sipPort']);

  // persisted + reloadable
  const reloaded = SipfaxConfig.load({ path }).config;
  assert.equal(reloaded.maxSessions, 8);
  assert.equal(reloaded.sip.sipPort, 5070);
  assert.equal(JSON.parse(readFileSync(path, 'utf8')).maxSessions, 8);
});

test('config user add/remove emits change and verifyAdmin checks the password', () => {
  const path = tmpConfigPath();
  const { config } = SipfaxConfig.load({ path, env: { SIPFAX_ADMIN_PASSWORD: 'pw' } });
  const events = [];
  config.on('change', (e) => events.push(e));
  config.addUser({ username: 'bob', password: 'p2' });
  assert.equal(config.ppp.users.find((u) => u.username === 'bob').password, 'p2');
  assert.deepEqual(events.at(-1).hot, ['ppp.users']);
  config.removeUser('bob');
  assert.equal(config.ppp.users.find((u) => u.username === 'bob'), undefined);
  assert.throws(() => config.removeUser('nope'));

  assert.equal(config.verifyAdmin('admin', 'pw'), true);
  assert.equal(config.verifyAdmin('admin', 'wrong'), false);
  assert.equal(config.verifyAdmin('root', 'pw'), false);
});

// ---------------------------------------------------------------- credentials
test('PppCredentialStore supports runtime add/remove and renders all users to secrets', () => {
  const store = new PppCredentialStore([{ username: 'a', password: 'pa' }]);
  store.addUser({ username: 'b', password: 'pb' });
  assert.equal(store.size, 2);
  assert.deepEqual(store.chapSecrets().map((s) => s.username).sort(), ['a', 'b']);
  assert.equal(store.removeUser('a'), true);
  assert.equal(store.removeUser('a'), false);
  assert.equal(store.size, 1);
  assert.deepEqual(store.chapSecrets().map((s) => s.username), ['b']);
});

// ---------------------------------------------------------------- multi-session
test('MultiSessionManager admits up to cap with distinct RTP ports and rejects beyond', () => {
  const pool = new RtpPortPool({ range: [42000, 42010] });
  const manager = new MultiSessionManager({
    publicHost: '198.51.100.5',
    rtpPortPool: pool,
    maxSessions: 2,
    lineFactory: (o) => new FakeLine(o)
  });
  const r1 = manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'c1' })));
  const r2 = manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'c2' })));
  const r3 = manager.startFromInvite(parseSipMessage(makeInvite({ callId: 'c3' })));
  assert.equal(r1.accepted, true);
  assert.equal(r2.accepted, true);
  assert.equal(r3.accepted, false);
  assert.equal(r3.statusCode, 486);
  assert.notEqual(r1.session.localRtpPort, r2.session.localRtpPort);
  const diag = manager.diagnostics();
  assert.equal(diag.active, 2);
  assert.equal(diag.limit, 2);
});

// ---------------------------------------------------------------- admin API
async function withOperator(config, fn) {
  const operator = new OperatorHttpServer({
    host: '127.0.0.1',
    port: 0,
    config,
    diagnostics: () => ({ sessions: { active: 0, limit: config.maxSessions, sessions: [] } }),
    freepbx: { serverHost: '127.0.0.1', sipPort: 5060, extension: 'fax' }
  });
  await operator.start();
  const { port } = operator.server.address();
  try {
    await fn(port);
  } finally {
    await operator.stop();
  }
}
const basic = (u, p) => 'Basic ' + Buffer.from(`${u}:${p}`).toString('base64');

test('admin API requires Basic auth and supports user/cap CRUD', async () => {
  const { config } = SipfaxConfig.load({ path: tmpConfigPath(), env: { SIPFAX_ADMIN_PASSWORD: 'pw' } });
  await withOperator(config, async (port) => {
    const base = `http://127.0.0.1:${port}`;
    // no auth -> 401
    assert.equal((await fetch(`${base}/admin/config`)).status, 401);
    // wrong auth -> 401
    assert.equal((await fetch(`${base}/admin/config`, { headers: { authorization: basic('admin', 'no') } })).status, 401);
    // good auth -> 200 + redacted
    const cfg = await fetch(`${base}/admin/config`, { headers: { authorization: basic('admin', 'pw') } });
    assert.equal(cfg.status, 200);
    const body = await cfg.json();
    assert.equal(body.maxSessions, config.maxSessions);

    // add user
    const add = await fetch(`${base}/admin/users`, {
      method: 'POST',
      headers: { authorization: basic('admin', 'pw'), 'content-type': 'application/json' },
      body: JSON.stringify({ username: 'carol', password: 'pc' })
    });
    assert.equal(add.status, 200);
    assert.ok(config.ppp.users.some((u) => u.username === 'carol'));

    // set cap
    const cap = await fetch(`${base}/admin/cap`, {
      method: 'PUT',
      headers: { authorization: basic('admin', 'pw'), 'content-type': 'application/json' },
      body: JSON.stringify({ maxSessions: 7 })
    });
    assert.equal(cap.status, 200);
    assert.equal(config.maxSessions, 7);

    // remove user
    const del = await fetch(`${base}/admin/users/carol`, {
      method: 'DELETE',
      headers: { authorization: basic('admin', 'pw') }
    });
    assert.equal(del.status, 200);
    assert.equal(config.ppp.users.some((u) => u.username === 'carol'), false);
  });
});

test('admin API returns 503 until an admin password is configured', async () => {
  const { config } = SipfaxConfig.load({ path: tmpConfigPath(), env: {} });
  assert.equal(config.adminConfigured(), false);
  await withOperator(config, async (port) => {
    assert.equal((await fetch(`http://127.0.0.1:${port}/admin/config`)).status, 503);
  });
});
