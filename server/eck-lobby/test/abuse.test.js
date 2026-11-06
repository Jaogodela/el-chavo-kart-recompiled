import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { PROTECTION, RateLimiter } from '../src/abuse.js';
import { COUNTRY_HEADER, LIMITED_HEADER, limitedCount } from '../src/admin.js';
import { handleUpgrade, upgradeAllowed } from '../src/edge.js';
import { Hub } from '../src/hub.js';
import { SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { LIMITS, ipGroup } from '../src/protocol.js';
import { abuse, befriend, countWrites, greeted, identity, joinRoom, makeHub, party, writesTo } from './harness.js';

const IP = '203.0.113.7';
const OTHER_IP = '198.51.100.4';

const fakeLobby = () => ({ seen: [], fetch(request) { this.seen.push(request); return Promise.resolve(new Response('ok')); } });
const fakeLimiter = (allow = () => true) => ({ keys: [], limit({ key }) { this.keys.push(key); return Promise.resolve({ success: allow(key) }); } });
const upgradeRequest = (headers = {}) => new Request('https://lobby.example/', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': '203.0.113.50', ...headers } });

const sessions = (env) => env.hub.metricsSummary().today.sessions;

function signedIn(env, n, options = {}) {
  const client = env.connect(identity(n), { ip: IP, ...options });
  assert.equal(client.hello().code, 'ok');
  return client;
}

function reconnect(env, n, options = {}) {
  const client = env.connect(identity(n), { ip: IP, ...options });
  return { client, reply: client.hello() };
}

test('the edge groups IPv4 and compressed or expanded IPv6 addresses by /64', () => {
  assert.equal(ipGroup('203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('::ffff:203.0.113.7'), '203.0.113.7');
  assert.equal(ipGroup('2001:db8:1:2::5'), ipGroup('2001:0DB8:0001:0002:aaaa:bbbb:cccc:dddd'));
  assert.equal(ipGroup('2001:db8::1'), ipGroup('2001:db8:0:0:9::1'));
  assert.notEqual(ipGroup('2001:db8:1:2::5'), ipGroup('2001:db8:1:3::5'));
  assert.equal(ipGroup('::1'), '0:0:0:0');
});

test('an upgrade over the edge limit is a 429 that never reaches the lobby', async () => {
  const lobby = fakeLobby();
  const limiter = fakeLimiter((key) => !key.includes('198.51.100.9'));
  const env = { WS_UPGRADE_LIMITER: limiter };
  const refused = await handleUpgrade(upgradeRequest({ 'CF-Connecting-IP': '198.51.100.9' }), env, lobby);
  assert.equal(refused.status, 429);
  assert.equal(refused.headers.get('Retry-After'), '60');
  assert.equal(lobby.seen.length, 0);
  assert.equal((await handleUpgrade(upgradeRequest(), env, lobby)).status, 200);
  assert.equal(limiter.keys[0], 'ws:198.51.100.9');
});

test('every address of one /64 shares an edge limit key', async () => {
  const limiter = fakeLimiter();
  await upgradeAllowed(limiter, '2001:db8:1:2::5');
  await upgradeAllowed(limiter, '2001:db8:1:2:ffff::1');
  await upgradeAllowed(limiter, '2001:db8:1:3::5');
  assert.equal(limiter.keys[0], limiter.keys[1]);
  assert.notEqual(limiter.keys[0], limiter.keys[2]);
});

test('refused upgrades are counted on the next one forwarded, and a client cannot forge the count or the country', async () => {
  const lobby = fakeLobby();
  let allow = false;
  const env = { WS_UPGRADE_LIMITER: fakeLimiter(() => allow) };
  for (let i = 0; i < 3; i++) await handleUpgrade(upgradeRequest(), env, lobby);
  allow = true;
  await handleUpgrade(upgradeRequest(), env, lobby);
  assert.equal(limitedCount(lobby.seen[0].headers), 3);
  await handleUpgrade(upgradeRequest({ [LIMITED_HEADER]: '999999', [COUNTRY_HEADER]: 'ZZ' }), env, lobby);
  assert.equal(lobby.seen[1].headers.get(LIMITED_HEADER), null);
  assert.equal(lobby.seen[1].headers.get(COUNTRY_HEADER), 'Unknown');
  for (const bad of ['-4', '1.5', 'x', '']) assert.equal(limitedCount(new Headers({ [LIMITED_HEADER]: bad })), 0);
});

test('without the binding, or if it fails, upgrades pass', async () => {
  assert.equal(await upgradeAllowed(undefined, IP), true);
  assert.equal(await upgradeAllowed({ limit: async () => { throw new Error('down'); } }, IP), true);
  assert.equal(await upgradeAllowed(fakeLimiter(), ''), true);
  assert.equal((await handleUpgrade(upgradeRequest(), {}, fakeLobby())).status, 200);
});

test('the Worker declares its own limiter, class and binding, isolated from every other service', () => {
  const root = join(import.meta.dirname, '..');
  const toml = readFileSync(join(root, 'wrangler.toml'), 'utf8');
  assert.match(toml, /^name = "eck-lobby"/m);
  assert.match(toml, /^main = "src\/worker\.js"/m);
  assert.match(toml, /name = "LOBBY"/);
  assert.match(toml, /class_name = "ECKLobby"/);
  assert.match(toml, /tag = "v1"/);
  assert.match(toml, /new_sqlite_classes = \["ECKLobby"\]/);
  assert.match(toml, /name = "WS_UPGRADE_LIMITER"/);
  assert.match(toml, /simple = \{ limit = 30, period = 60 \}/);
  assert.equal(/namespace_id = "(\d+)"/.exec(toml)[1], '4004');
  const worker = readFileSync(join(root, 'src/worker.js'), 'utf8');
  assert.match(worker, /export class ECKLobby extends DurableObject/);
  assert.ok(worker.indexOf('isAdminPath(url.pathname)') < worker.indexOf('handleUpgrade(request'));
  assert.ok(worker.indexOf('handleUpgrade(request') < worker.indexOf("url.pathname === '/health'"));
});

test('the lobby counts what the edge refused and the upgrades it accepted, in memory until the alarm', () => {
  const env = makeHub();
  const writes = countWrites(env);
  env.hub.edgeLimited(7);
  env.hub.edgeLimited(0);
  env.connect(identity(1));
  assert.equal(writesTo(writes, 'metrics_daily_dimensions').length, 0);
  env.advance(SAMPLE_MS);
  assert.deepEqual(abuse(env), { ws_upgrade_accepted: 1, ws_upgrade_limited: 7 });
});

test('sockets of one address are capped, loopback is exempt only for the development server', () => {
  const env = makeHub({ local: false });
  const clients = Array.from({ length: LIMITS.socketsPerIp + 2 }, (_, i) => env.connect(identity(i + 1), { ip: IP }));
  assert.equal(clients.filter((c) => c.admitted).length, LIMITS.socketsPerIp);
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).ws_upgrade_ip_cap, 2);
  const local = makeHub({ local: true });
  const many = Array.from({ length: 40 }, (_, i) => local.connect(identity(i + 1), { ip: '127.0.0.1' }));
  assert.equal(many.every((c) => c.admitted), true);
});

test('sockets of one /64 share the per-address cap whatever the spelling', () => {
  const env = makeHub({ local: false });
  const spellings = ['2001:db8:1:2::5', '2001:0db8:0001:0002:0:0:0:6', '2001:DB8:1:2:ffff::7', '2001:db8:1:2::8'];
  for (let i = 0; i < LIMITS.socketsPerIp; i++) assert.ok(env.connect(identity(i + 1), { ip: spellings[i % 4] }).admitted);
  assert.equal(env.connect(identity(40), { ip: '2001:db8:1:2:1::9' }).admitted, false);
  assert.ok(env.connect(identity(41), { ip: '2001:db8:1:3::9' }).admitted);
});

test('the limiter refills, refuses without spending, and stays bounded', () => {
  const limiter = new RateLimiter({ burst: 2, perSecond: 1 }, 8);
  assert.ok(limiter.take('a', 0) && limiter.take('a', 0));
  assert.equal(limiter.take('a', 0), false);
  assert.equal(limiter.allows('a', 1000), true);
  assert.equal(limiter.take('a', 1000), true);
  for (let i = 0; i < 100; i++) limiter.take(`k${i}`, 5000);
  assert.ok(limiter.buckets.size <= 8);
});

test('reconnecting does not rewrite the player row or the session metrics', () => {
  const env = makeHub({ local: false });
  let client = signedIn(env, 1);
  const writes = countWrites(env);
  for (let i = 0; i < 5; i++) {
    client.drop();
    env.clock.now += 30_000;
    const next = reconnect(env, 1);
    assert.equal(next.reply.code, 'ok');
    client = next.client;
  }
  assert.deepEqual(writes.filter((q) => /players/.test(q)), []);
  assert.deepEqual(writes.filter((q) => q.includes('metrics_daily') && !q.includes("'abuse'")), []);
});

test('the player row is refreshed when the name changes or the last write is old, and not otherwise', () => {
  const env = makeHub({ local: false });
  const first = signedIn(env, 1, { name: 'Alice' });
  first.drop();
  const writes = countWrites(env);
  const probe = { name: 'Alicia', presence: false };
  const renamed = reconnect(env, 1, probe);
  assert.equal(renamed.reply.name, 'Alicia');
  assert.equal(writesTo(writes, 'UPDATE players').length, 1);
  renamed.client.drop();
  env.clock.now += PROTECTION.seenRefreshMs - 1000;
  reconnect(env, 1, probe).client.drop();
  assert.equal(writesTo(writes, 'UPDATE players').length, 1);
  env.clock.now += 1000;
  reconnect(env, 1, probe);
  assert.equal(writesTo(writes, 'UPDATE players').length, 2);
  assert.equal(env.hub.playerRow(first.who.player).seen, env.clock.now);
});

test('a token that does not match the player is refused and writes nothing', () => {
  const env = makeHub({ local: false });
  const owner = signedIn(env, 1);
  const writes = countWrites(env);
  const thief = env.connect({ player: owner.who.player, token: 'cd'.repeat(32) }, { ip: IP });
  assert.equal(thief.hello().code, 'unauthorized');
  assert.equal(writes.length, 0);
});

test('the raw token is never stored and a player id that is not derived from it is refused', () => {
  const env = makeHub({ local: false });
  const owner = signedIn(env, 1);
  const stored = JSON.stringify(env.host.sql.all('SELECT * FROM players'));
  assert.ok(!stored.includes(owner.who.token));
  const forged = env.connect({ player: identity(2).player, token: 'cd'.repeat(32) }, { ip: IP });
  assert.equal(forged.hello().code, 'unauthorized');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM players').n, 1);
});

test('a rapid reconnect of one player is one session; after the window it is another', () => {
  const env = makeHub({ local: false });
  let client = signedIn(env, 1);
  assert.equal(sessions(env), 1);
  for (let i = 0; i < 4; i++) {
    client.drop();
    env.clock.now += 5_000;
    client = reconnect(env, 1).client;
  }
  assert.equal(sessions(env), 1);
  client.drop();
  env.clock.now += PROTECTION.sessionWindowMs;
  assert.equal(reconnect(env, 1).reply.code, 'ok');
  assert.equal(sessions(env), 2);
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).hello_reconnect_deduped, 4);
  assert.equal(abuse(env).hello_accepted, 6);
});

test('the Connection test does not use up the session window', () => {
  const env = makeHub({ local: false });
  const owner = env.connect(identity(1), { ip: IP, presence: false });
  assert.equal(owner.hello().code, 'ok');
  assert.equal(sessions(env), 0);
  owner.drop();
  assert.equal(reconnect(env, 1).reply.code, 'ok');
  assert.equal(sessions(env), 1);
});

test('a hello flood on one player is limited; guessing at a player cannot lock its owner out', () => {
  const env = makeHub({ local: false });
  const who = identity(1);
  const results = [];
  for (let i = 0; i < PROTECTION.hello.burst + 3; i++) {
    const c = env.connect(who, { ip: IP });
    results.push(c.hello().code);
    c.drop();
  }
  assert.equal(results.at(-1), 'limit');
  assert.ok(results.slice(0, PROTECTION.hello.burst).every((r) => r === 'ok'));
  const writes = countWrites(env);
  assert.equal(reconnect(env, 1).reply.code, 'limit');
  assert.equal(writes.length, 0);
  const other = signedIn(env, 2);
  other.drop();
  for (let i = 0; i < 40; i++) {
    const guess = env.connect({ token: identity(9).token, player: other.who.player }, { ip: IP });
    assert.equal(guess.hello().code, 'unauthorized');
    guess.drop();
  }
  assert.equal(reconnect(env, 2).reply.code, 'ok');
});

test('too many new players from one address are refused, and each new address starts fresh', () => {
  const env = makeHub({ local: false });
  let refused = 0;
  for (let i = 0; i < LIMITS.registrationsPerIpPerDay + 3; i++) {
    const c = env.connect(identity(100 + i), { ip: IP });
    if (c.hello().code === 'limit') refused += 1;
    c.drop();
  }
  assert.equal(refused, 3);
  assert.equal(env.connect(identity(900), { ip: OTHER_IP }).hello().code, 'ok');
});

test('the development server is not limited on loopback', () => {
  const env = makeHub({ local: true });
  const first = signedIn(env, 1, { ip: '127.0.0.1' });
  first.drop();
  for (let i = 0; i < 30; i++) {
    const next = reconnect(env, 1, { ip: '127.0.0.1' });
    assert.equal(next.reply.code, 'ok');
    next.client.drop();
  }
});

test('message size, envelope and rate limits answer with codes instead of work', () => {
  const env = makeHub({ local: false });
  const a = signedIn(env, 1);
  assert.equal(a.raw(JSON.stringify({ op: 'ping', seq: 1, pad: 'x'.repeat(LIMITS.frameBytes) })).code, 'too_large');
  assert.equal(a.raw('[]').code, 'bad_request');
  assert.equal(a.raw(JSON.stringify({ op: 'ping', seq: -1 })).code, 'bad_request');
  let limited = 0;
  for (let i = 0; i < 200; i++) if (a.call('ping').code === 'limit') limited += 1;
  assert.ok(limited > 100);
  env.clock.now += 10_000;
  assert.equal(a.call('ping').code, 'ok');
});

test('hosting in a loop is limited, reconnecting does not reset it, and searching and leaving still work', () => {
  const env = makeHub({ local: false });
  let client = signedIn(env, 1);
  const { burst } = PROTECTION.hostAccount;
  for (let i = 0; i < burst; i++) {
    env.clock.now += 1100;
    const reply = client.call('room.host', {});
    assert.equal(reply.code, 'ok', `round ${i}`);
    assert.equal(client.call('room.cancel', { room: reply.room.room }).code, 'ok');
  }
  env.clock.now += 1100;
  assert.equal(client.call('room.host', {}).code, 'limit');
  client.drop();
  client = reconnect(env, 1).client;
  assert.equal(client.call('room.host', {}).code, 'limit');
  assert.equal(client.call('room.search').code, 'ok');
  assert.equal(client.call('room.leave', { room: 'none' }).code, 'ok');
  env.clock.now += 20_000;
  assert.equal(client.call('room.host', {}).code, 'ok');
  env.advance(SAMPLE_MS);
  assert.deepEqual([abuse(env).room_host_accepted, abuse(env).room_host_limited], [burst + 1, 2]);
});

test('a refused room.host writes nothing', () => {
  const env = makeHub({ local: false });
  const client = signedIn(env, 1);
  for (let i = 0; i < PROTECTION.hostAccount.burst; i++) {
    env.clock.now += 1100;
    client.call('room.cancel', { room: client.call('room.host', {}).room.room });
  }
  env.clock.now += 1100;
  const writes = countWrites(env);
  assert.equal(client.call('room.host', {}).code, 'limit');
  assert.deepEqual(writes, []);
});

test('players behind one address share a wider hosting limit; other addresses are unaffected', () => {
  const env = makeHub({ local: false });
  const codes = [];
  const total = PROTECTION.hostAddress.burst + 3;
  for (let i = 0; i < total; i++) signedIn(env, 200 + i, { ip: `192.0.2.${i + 1}` }).drop();
  for (let i = 0; i < total; i++) {
    const { client } = reconnect(env, 200 + i, { ip: '203.0.113.50' });
    codes.push(client.call('room.host', {}).code);
    client.drop();
  }
  assert.equal(codes.filter((c) => c === 'ok').length, PROTECTION.hostAddress.burst);
  assert.equal(codes.at(-1), 'limit');
  assert.equal(signedIn(env, 99, { ip: '203.0.113.51' }).call('room.host', {}).code, 'ok');
});

test('refreshing an unchanged room writes only when the last write is old and spends no creation budget', () => {
  const env = makeHub({ local: false });
  const host = signedIn(env, 1);
  const { room } = host.call('room.host', { capacity: 3, rules: { laps: 3 } }).room;
  const before = env.hub.guards.hostAccount.tokens(host.who.player, env.clock.now);
  const writes = countWrites(env);
  env.clock.now += PROTECTION.advertiseRefreshMs - 1000;
  assert.equal(host.call('room.host', { capacity: 3, rules: { laps: 3 } }).room.room, room);
  assert.deepEqual(writes, []);
  env.clock.now += 1000;
  assert.equal(host.call('room.host', { capacity: 3, rules: { laps: 3 } }).room.room, room);
  assert.equal(writesTo(writes, 'UPDATE rooms').length, 1);
  assert.equal(host.call('room.host', { capacity: 3, rules: { laps: 5 } }).room.rules.laps, 5);
  assert.equal(writesTo(writes, 'UPDATE rooms').filter((q) => q.includes('rules')).length, 1);
  assert.ok(env.hub.guards.hostAccount.tokens(host.who.player, env.clock.now) >= before);
  assert.equal(host.events('room.created').length, 1);
});

test('a refreshed open room lives on from its last refresh; a host that stops refreshing is closed', () => {
  const env = makeHub({ local: false });
  const host = signedIn(env, 1);
  host.call('room.host', {});
  for (let i = 0; i < 3; i++) {
    env.advance(LIMITS.openRoomMs / 2);
    assert.equal(host.call('room.host', {}).code, 'ok');
  }
  assert.deepEqual(host.events('room.closed'), []);
  env.advance(LIMITS.openRoomMs + 1000);
  assert.equal(host.events('room.closed')[0].reason, 'expired');
});

test('room.update that changes nothing writes nothing and tells nobody', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 2, 3);
  const writes = countWrites(env);
  assert.equal(host.call('room.update', { room, capacity: 3 }).room.capacity, 3);
  assert.deepEqual(writes, []);
  assert.deepEqual(guests[0].events('room.updated'), []);
});

test('searching again writes nothing while the handles have time left', () => {
  const env = makeHub();
  const { host } = party(env, 1, 4);
  const seeker = greeted(env, 40);
  befriend(host, seeker);
  const first = seeker.call('room.search').rooms[0].handle;
  const writes = countWrites(env);
  for (let i = 0; i < 20; i++) assert.equal(seeker.call('room.search').rooms[0].handle, first);
  assert.deepEqual(writes, []);
  env.clock.now += LIMITS.handleMs / 2 + 1;
  assert.notEqual(seeker.call('room.search').rooms[0].handle, first);
  assert.equal(writesTo(writes, 'handles').length, 1);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM handles').n, 1);
});

test('search results are bounded and only list what the caller may join', () => {
  const env = makeHub();
  const seeker = greeted(env, 1);
  for (let i = 0; i < LIMITS.searchResults + 5; i++) {
    const host = greeted(env, 10 + i);
    befriend(host, seeker);
    host.call('room.host', {});
  }
  assert.equal(seeker.call('room.search').rooms.length, LIMITS.searchResults);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM handles').n, LIMITS.searchResults);
  seeker.call('room.search');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM handles').n, LIMITS.searchResults);
});

test('joining in a loop is limited, and failed joins do not count', () => {
  const env = makeHub({ local: false });
  const joiner = signedIn(env, 1, { ip: OTHER_IP });
  for (let i = 0; i < 30; i++) {
    assert.equal(joiner.call('room.join', { handle: 'nope' }).code, 'not_found');
    env.clock.now += 1000;
  }
  assert.equal(env.hub.guards.joinAccount.tokens(joiner.who.player, env.clock.now), PROTECTION.joinAccount.burst);
  const host = signedIn(env, 2);
  befriend(host, joiner);
  const { room } = host.call('room.host', { capacity: 4 }).room;
  const { handle } = joiner.call('room.search').rooms[0];
  const { joinAccount } = env.hub.guards;
  while (joinAccount.take(joiner.who.player, env.clock.now));
  assert.equal(joiner.call('room.join', { handle }).code, 'limit');
  env.clock.now += 6000;
  assert.equal(joiner.call('room.join', { handle }).code, 'ok');
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).room_join_limited, 1);
  assert.ok(room);
});

test('two connections taking a seat from each other are limited', () => {
  const env = makeHub({ local: false });
  const host = signedIn(env, 1);
  const guest = signedIn(env, 2, { ip: OTHER_IP });
  befriend(host, guest);
  const { room } = host.call('room.host', {}).room;
  joinRoom(host, guest);
  guest.drop();
  const sockets = [signedIn(env, 2, { ip: OTHER_IP }), signedIn(env, 2, { ip: OTHER_IP })];
  const codes = [];
  for (let i = 0; i < PROTECTION.resumeAccount.burst + 4; i++) {
    env.clock.now += 1100;
    codes.push(sockets[i % 2].call('match.resume', { room }).code);
  }
  assert.ok(codes.slice(0, PROTECTION.resumeAccount.burst).every((c) => c === 'ok'));
  assert.equal(codes.at(-1), 'limit');
  env.advance(SAMPLE_MS);
  assert.ok(abuse(env).room_resume_limited >= 1);
});

function inviter(env, nHost, nTarget, ips = {}) {
  const host = signedIn(env, nHost, { ip: ips.host ?? IP });
  const target = signedIn(env, nTarget, { ip: ips.target ?? OTHER_IP });
  befriend(host, target);
  assert.equal(host.call('room.host', {}).code, 'ok');
  return { host, target };
}

test('inviting in a loop is limited, a second connection shares the limit, and refusals write nothing', () => {
  const env = makeHub({ local: false });
  const { host, target } = inviter(env, 1, 2);
  const codes = [];
  for (let i = 0; i < 80 && codes.at(-1) !== 'limit'; i++) {
    env.clock.now += 1000;
    codes.push(host.call('invite.send', { player: target.who.player }).code);
  }
  assert.equal(codes.at(-1), 'limit');
  assert.ok(codes.filter((c) => c === 'ok').length >= PROTECTION.inviteAccount.burst);
  const second = reconnect(env, 1).client;
  const room = env.hub.roomOfPlayer(host.who.player);
  assert.ok(room);
  const writes = countWrites(env);
  assert.equal(host.call('invite.send', { player: target.who.player }).code, 'limit');
  assert.deepEqual(writes, []);
  env.advance(SAMPLE_MS);
  assert.ok(abuse(env).invite_limited >= 2);
  assert.ok(second);
});

test('invites that fail validation spend no invite budget', () => {
  const env = makeHub({ local: false });
  const host = signedIn(env, 1);
  const stranger = signedIn(env, 2, { ip: OTHER_IP });
  host.call('room.host', {});
  for (let i = 0; i < 40; i++) {
    assert.equal(host.call('invite.send', { player: stranger.who.player }).code, 'not_friends');
    env.clock.now += 1000;
  }
  assert.equal(env.hub.guards.inviteAccount.tokens(host.who.player, env.clock.now), PROTECTION.inviteAccount.burst);
});

test('players behind one address share a wider invite limit', () => {
  const env = makeHub({ local: false });
  const pairs = [1, 2, 3, 4].map((n) => inviter(env, n, n + 10, { host: IP, target: `198.51.100.${n}` }));
  let limited = null;
  for (let round = 0; round < 40 && !limited; round++) {
    env.clock.now += 1000;
    for (const { host, target } of pairs) {
      if (host.call('invite.send', { player: target.who.player }).code === 'limit') limited = host;
    }
  }
  assert.ok(limited);
  assert.equal(env.hub.guards.inviteAccount.allows(limited.who.player, env.clock.now), true);
  assert.equal(env.hub.guards.inviteAddress.allows(IP, env.clock.now), false);
});

test('repeating an unchanged endpoint, ready flag or transport choice writes nothing', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 2);
  const [guest] = guests;
  assert.equal(host.call('match.endpoint', { room, port: 7101 }).code, 'ok');
  assert.equal(guest.call('match.endpoint', { room, port: 7102 }).code, 'ok');
  assert.equal(host.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'relay' }).code, 'ok');
  guest.inbox = [];
  const writes = countWrites(env);
  assert.equal(host.call('match.endpoint', { room, port: 7101 }).code, 'ok');
  assert.equal(host.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'relay' }).code, 'ok');
  assert.deepEqual(writes, []);
  assert.equal(guest.events('match.peer').length, 1);
  assert.equal(guest.events('match.transport').length, 1);
  assert.deepEqual(guest.events('room.updated'), []);
  assert.equal(host.call('match.endpoint', { room, port: 7103 }).code, 'ok');
  assert.equal(host.call('match.ready', { room, ready: false }).code, 'ok');
  assert.equal(writesTo(writes, 'UPDATE room_seats').length, 2);
});

test('a woken object that already knows its alarm does not set it again', () => {
  const env = makeHub();
  greeted(env, 1);
  assert.notEqual(env.host.alarmAt, null);
  const sets = [];
  const setAlarm = env.host.setAlarm.bind(env.host);
  env.host.setAlarm = (ms) => {
    sets.push(ms);
    setAlarm(ms);
  };
  void new Hub(env.host, { alarmAt: env.host.alarmAt });
  assert.equal(sets.length, 0);
  void new Hub(env.host);
  assert.equal(sets.length, 1);
});

test('activity that does not move the next deadline earlier does not set the alarm again', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('room.host', {});
  const sets = [];
  const setAlarm = env.host.setAlarm.bind(env.host);
  env.host.setAlarm = (ms) => {
    sets.push(ms);
    setAlarm(ms);
  };
  greeted(env, 2);
  a.call('room.host', {});
  assert.equal(sets.length, 0);
});

test('diagnostics flush once per interval and an idle object schedules no further alarm', () => {
  const env = makeHub();
  env.hub.edgeLimited(3);
  assert.equal(env.host.alarmAt, env.clock.now - (env.clock.now % SAMPLE_MS) + SAMPLE_MS);
  const writes = countWrites(env);
  env.hub.edgeLimited(2);
  assert.deepEqual(writes, []);
  env.advance(SAMPLE_MS);
  assert.deepEqual(abuse(env), { ws_upgrade_limited: 5 });
  assert.equal(env.host.alarmAt, null);
});

test('the alarm sweeps old requests, registrations and handles', () => {
  const env = makeHub({ local: false });
  const a = signedIn(env, 1);
  const b = signedIn(env, 2, { ip: OTHER_IP });
  a.call('friends.add', { code: b.who.player });
  env.host.sql.run('INSERT INTO handles (handle, room, viewer, expires) VALUES (?, ?, ?, ?)', 'beef', 'gone', a.who.player, env.clock.now);
  env.clock.now += 15 * 24 * 3600 * 1000;
  env.hub.alarm();
  for (const table of ['friend_requests', 'registrations', 'handles']) {
    assert.equal(env.host.sql.get(`SELECT COUNT(*) AS n FROM ${table}`).n, 0, table);
  }
});

test('abuse diagnostics hold counter names only', () => {
  const env = makeHub({ local: false });
  const client = signedIn(env, 1);
  for (let i = 0; i < 9; i++) {
    env.clock.now += 1100;
    client.call('room.cancel', { room: client.call('room.host', {}).room?.room });
  }
  env.advance(SAMPLE_MS);
  const rows = env.host.sql.all(`SELECT * FROM metrics_daily_dimensions WHERE dimension = 'abuse' AND day = ?`, metricsDay(env.clock.now));
  const text = JSON.stringify(rows);
  assert.ok(rows.length > 0);
  assert.ok(!text.includes(client.who.player) && !text.includes(IP) && !text.includes(client.who.token));
  assert.deepEqual(rows.map((r) => r.value).filter((v) => !/^[a-z0-9_]+$/.test(v)), []);
});

test('a room open for an hour, and any room after four, is closed by the service', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  a.call('room.host', {});
  env.advance(LIMITS.openRoomMs + 1000);
  assert.equal(a.events('room.closed')[0].reason, 'expired');
  const { room } = a.call('room.host', {}).room;
  const { handle } = b.call('room.search').rooms[0];
  b.call('room.join', { handle });
  a.call('room.start', { room });
  env.advance(LIMITS.openRoomMs + 1000);
  assert.deepEqual(b.events('room.closed'), []);
  env.advance(LIMITS.roomMaxMs);
  assert.equal(b.events('room.closed')[0].reason, 'expired');
});
