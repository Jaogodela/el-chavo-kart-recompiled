// Bounded abuse scenarios: each one floods the in-process hub, then checks that the limits engaged, that SQL writes stayed
// within what the limits allow, that metrics stayed aggregate, and that a normal player was not affected.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { LIMITS, RELAY, encodeRelayFrame } from '../src/protocol.js';
import { SAMPLE_MS } from '../src/metrics.js';
import { abuse, befriend, countWrites, greeted, identity, joinRoom, makeHub, writesTo } from './harness.js';

const ATTACKER = '203.0.113.66';
const PLAYER = '198.51.100.8';

function bystanders(env) {
  const host = greeted(env, 500, { ip: PLAYER });
  const guest = greeted(env, 501, { ip: '198.51.100.9' });
  befriend(host, guest);
  return { host, guest };
}

function assertNormalPlayWorks({ host, guest }) {
  assert.equal(host.call('room.host', { capacity: 2 }).code, 'ok');
  assert.equal(joinRoom(host, guest).code, 'ok');
  const room = host.hub.roomOfPlayer(host.who.player).id;
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.equal(host.call('room.leave', { room }).code, 'ok');
}

function assertAggregate(env, secrets) {
  const rows = JSON.stringify(['metrics_daily', 'metrics_daily_dimensions', 'metrics_samples', 'recent_rooms']
    .map((table) => env.host.sql.all(`SELECT * FROM ${table}`)));
  for (const secret of secrets) assert.ok(!rows.includes(secret), secret);
}

test('a reconnect loop writes nothing per turn and counts as one session', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const who = identity(1);
  const first = env.connect(who, { ip: ATTACKER });
  first.hello();
  first.drop();
  const writes = countWrites(env);
  let refused = 0;
  for (let i = 0; i < 500; i++) {
    const c = env.connect(who, { ip: ATTACKER });
    if (c.hello().code === 'limit') refused += 1;
    c.drop();
  }
  assert.equal(refused, 500 - PROTECTION.hello.burst + 1);
  assert.equal(writesTo(writes, 'players').length, 0);
  assert.equal(writesTo(writes, 'metrics_daily').length, 0);
  assert.equal(env.hub.metricsSummary().today.sessions, 3);
  assertNormalPlayWorks(people);
  env.advance(SAMPLE_MS);
  assertAggregate(env, [who.player, ATTACKER, who.token]);
});

test('hello spam under many identities is stopped by the socket cap and the registration limit', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const writes = countWrites(env);
  let registered = 0;
  let capped = 0;
  for (let i = 0; i < 300; i++) {
    const c = env.connect(identity(1000 + i), { ip: ATTACKER });
    if (!c.admitted) {
      capped += 1;
      continue;
    }
    if (c.hello().code === 'ok') registered += 1;
    if (i % 2) c.drop();
  }
  assert.equal(registered, LIMITS.registrationsPerIpPerDay);
  assert.ok(capped > 0);
  assert.equal(writesTo(writes, 'INSERT INTO players').length, LIMITS.registrationsPerIpPerDay);
  assertNormalPlayWorks(people);
});

test('room create spam is bounded and refreshing a room forever writes almost nothing', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const spammer = greeted(env, 1, { ip: ATTACKER });
  const writes = countWrites(env);
  let created = 0;
  for (let i = 0; i < 300; i++) {
    env.clock.now += 1100;
    const reply = spammer.call('room.host', { capacity: 4 });
    if (reply.code !== 'ok') continue;
    created += 1;
    spammer.call('room.cancel', { room: reply.room.room });
  }
  const allowed = PROTECTION.hostAccount.burst + Math.ceil(330 * PROTECTION.hostAccount.perSecond);
  assert.ok(created >= PROTECTION.hostAccount.burst && created <= allowed, `created ${created}`);
  assert.equal(writesTo(writes, 'INSERT INTO rooms').length, created);
  const refresher = greeted(env, 2, { ip: PLAYER });
  refresher.call('room.host', {});
  const refreshes = countWrites(env);
  for (let i = 0; i < 300; i++) {
    env.clock.now += 1000;
    assert.equal(refresher.call('room.host', {}).code, 'ok');
  }
  const windows = Math.ceil(300 * 1000 / PROTECTION.advertiseRefreshMs);
  assert.ok(writesTo(refreshes, 'UPDATE rooms').length <= windows, `${writesTo(refreshes, 'UPDATE rooms').length} writes`);
  refresher.call('room.cancel', { room: env.hub.roomOfPlayer(refresher.who.player).id });
  assertNormalPlayWorks(people);
});

test('room join spam spends one budget however the rooms come', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const joiner = greeted(env, 1, { ip: ATTACKER });
  const host = greeted(env, 2, { ip: PLAYER });
  befriend(host, joiner);
  const { room } = host.call('room.host', { capacity: 4 }).room;
  let accepted = 0;
  let limited = 0;
  for (let i = 0; i < 200; i++) {
    env.clock.now += 1100;
    const code = joinRoom(host, joiner).code;
    if (code === 'ok') {
      accepted += 1;
      joiner.call('room.leave', { room });
    } else if (code === 'limit') {
      limited += 1;
    }
  }
  const allowed = PROTECTION.joinAccount.burst + Math.ceil(220 * PROTECTION.joinAccount.perSecond);
  assert.ok(limited > 0 && accepted <= allowed, `accepted ${accepted}, limited ${limited}`);
  assert.equal(env.hub.metricsSummary().today.joins, accepted);
  host.call('room.cancel', { room });
  assertNormalPlayWorks(people);
  env.advance(SAMPLE_MS);
  assert.ok(abuse(env).room_join_limited >= 1);
});

test('invite spam stops at the limit and cannot grow the invite table', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const host = greeted(env, 1, { ip: ATTACKER });
  const friends = [2, 3, 4].map((n, i) => greeted(env, n, { ip: `192.0.2.${i + 1}` }));
  for (const f of friends) befriend(host, f);
  host.call('room.host', { capacity: 4 });
  const writes = countWrites(env);
  let sent = 0;
  for (let i = 0; i < 300; i++) {
    env.clock.now += 1100;
    if (host.call('invite.send', { player: friends[i % 3].who.player }).code === 'ok') sent += 1;
    assert.ok(env.host.sql.get('SELECT COUNT(*) AS n FROM invites').n <= LIMITS.invitesPerRoom);
  }
  const allowed = PROTECTION.inviteAccount.burst + Math.ceil(330 * PROTECTION.inviteAccount.perSecond);
  assert.ok(sent <= allowed, `sent ${sent}`);
  assert.equal(writesTo(writes, 'INSERT INTO invites').length, sent);
  assertNormalPlayWorks(people);
  env.advance(SAMPLE_MS);
  assert.ok(abuse(env).invite_limited > 0);
});

test('invalid relay spam closes the sender after a bounded number of frames, without a single write', () => {
  const env = makeHub({ local: false });
  const people = bystanders(env);
  const a = greeted(env, 1, { ip: ATTACKER });
  const b = greeted(env, 2, { ip: PLAYER });
  befriend(a, b);
  const hosted = a.call('room.host', {});
  joinRoom(a, b);
  const writes = countWrites(env);
  const forged = encodeRelayFrame(hosted.relay_token ^ 1, 1, 1, Uint8Array.from([1, 2, 3]));
  let sent = 0;
  for (; sent < 5000 && !a.closedWith; sent += 1) a.relay(forged);
  assert.equal(sent, RELAY.invalid.burst + 1);
  assert.equal(a.closedWith.code, 1008);
  for (let i = 0; i < 1000; i++) a.relay(forged);
  assert.deepEqual(writes.filter((q) => !q.includes('rooms')), []);
  assert.equal(b.binary.length, 0);
  assertNormalPlayWorks(people);
  env.advance(SAMPLE_MS);
  assert.equal(abuse(env).relay_abuse_closed, 1);
  assertAggregate(env, [a.who.player, ATTACKER, String(hosted.relay_token)]);
});
