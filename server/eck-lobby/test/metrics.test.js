import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { DIMENSIONS, HISTORY_MAX_DAYS, RECENT_ROOMS_DAYS, RECENT_ROOMS_MAX, SAMPLE_DAYS, SAMPLE_MS, countryCode, metricsDay,
  percentChange, platformCategory, versionCategory } from '../src/metrics.js';
import { countWrites, befriend, greeted, identity, joinRoom, makeHub, party, writesTo } from './harness.js';

const DAY = 24 * 3600 * 1000;
const dimension = (env, name) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', metricsDay(env.clock.now), name)
  .map((r) => [r.value, r.count]));

test('days group by the Sao Paulo calendar, not UTC', () => {
  assert.equal(metricsDay(Date.parse('2026-03-02T02:30:00Z')), '2026-03-01');
  assert.equal(metricsDay(Date.parse('2026-03-02T03:30:00Z')), '2026-03-02');
});

test('categories collapse what is unknown or unbounded', () => {
  assert.equal(platformCategory('Windows'), 'Windows');
  assert.equal(platformCategory('TempleOS'), 'Other');
  assert.equal(versionCategory('0.1.0'), '0.1.0');
  assert.equal(versionCategory(''), 'Unknown');
  assert.equal(versionCategory('<script>'), 'Other');
  assert.equal(countryCode('BR'), 'BR');
  for (const bad of ['XX', 'br', 'BRA', '', null]) assert.equal(countryCode(bad), 'Unknown');
  assert.deepEqual(percentChange(5, 0), null);
  assert.equal(percentChange(150, 100), 50);
});

test('one version per user cannot grow the dimension table without bound', () => {
  const env = makeHub();
  for (let i = 0; i < 90; i++) {
    const c = env.connect(identity(i + 1), { version: `9.${i}.0` });
    assert.equal(c.hello().code, 'ok');
    c.drop();
  }
  const versions = dimension(env, 'version');
  assert.equal(Object.keys(versions).length, 65);
  assert.equal(versions.Other, 90 - 64);
  const total = Object.values(versions).reduce((a, b) => a + b, 0);
  assert.equal(total, 90);
});

test('an unknown platform, version or country cannot become a value', () => {
  const env = makeHub();
  const c = env.connect(identity(1), { platform: 'Haiku', version: 'build abc', country: 'not a country' });
  assert.equal(c.hello().code, 'ok');
  assert.deepEqual(dimension(env, 'platform'), { Other: 1 });
  assert.deepEqual(dimension(env, 'version'), { Other: 1 });
  assert.deepEqual(dimension(env, 'country'), { Unknown: 1 });
});

test('the daily counters follow a room from creation to close', () => {
  const env = makeHub();
  const { host, clients, room } = party(env, 3, 4);
  const extra = greeted(env, 9);
  befriend(host, extra);
  host.call('invite.send', { player: extra.who.player });
  host.call('match.transport', { room, seat: 1, transport: 'direct' });
  host.call('match.transport', { room, seat: 2, transport: 'relay' });
  host.call('room.start', { room });
  for (const c of clients) c.call('match.ready', { room, ready: true });
  env.advance(90 * 1000);
  host.call('room.leave', { room });
  const { today } = env.hub.metricsSummary();
  assert.deepEqual([today.sessions, today.rooms_created, today.rooms_started, today.rooms_closed, today.joins, today.invites],
    [4, 1, 1, 1, 2, 1]);
  assert.deepEqual([today.direct_selected, today.relay_selected], [1, 1]);
  assert.deepEqual([today.rooms_with_guest, today.rooms_direct, today.rooms_relay, today.rooms_pending], [1, 0, 1, 0]);
  assert.deepEqual(dimension(env, 'transport'), { direct: 1, relay: 1 });
  assert.deepEqual(dimension(env, 'room_capacity'), { 4: 1 });
  assert.deepEqual(dimension(env, 'room_occupancy'), { '3/4': 1 });
  assert.deepEqual(dimension(env, 'room_lifetime'), { '1_5m': 1 });
  assert.deepEqual([today.races_started, today.races_completed], [0, 0]);
});

test('every occupancy bucket is reachable and a lone host is not one', () => {
  const env = makeHub();
  let first = 10;
  for (const [size, capacity] of [[2, 2], [2, 3], [3, 3], [2, 4], [3, 4], [4, 4]]) {
    const { host, room } = party(env, size, capacity, { first });
    first += 10;
    host.call('room.leave', { room });
  }
  const alone = greeted(env, 90);
  alone.call('room.cancel', { room: alone.call('room.host', { capacity: 4 }).room.room });
  assert.deepEqual(dimension(env, 'room_occupancy'), { '2/2': 1, '2/3': 1, '3/3': 1, '2/4': 1, '3/4': 1, '4/4': 1 });
  const { today } = env.hub.metricsSummary();
  assert.deepEqual([today.rooms_closed, today.rooms_with_guest, today.rooms_pending], [7, 6, 6]);
});

test('a room that shrank reports an occupancy that fits its size', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3, 4);
  guests[1].call('room.leave', { room });
  host.call('room.update', { room, capacity: 2 });
  host.call('room.leave', { room });
  assert.deepEqual(dimension(env, 'room_occupancy'), { '2/2': 1 });
});

test('a seat counts its transport once, however often the choice is repeated or changed', () => {
  const env = makeHub();
  const { host, room } = party(env, 2);
  for (const transport of ['relay', 'relay', 'direct', 'relay']) host.call('match.transport', { room, seat: 1, transport });
  assert.deepEqual(dimension(env, 'transport'), { relay: 1 });
});

test('recent rooms keep anonymous operational fields only', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3, 4, { options: { platform: 'Linux' } });
  host.call('match.transport', { room, seat: 1, transport: 'direct' });
  host.call('match.transport', { room, seat: 2, transport: 'relay' });
  host.call('room.start', { room });
  for (const c of [host, ...guests]) c.call('match.ready', { room, ready: true });
  env.advance(5 * 60 * 1000);
  host.call('room.leave', { room });
  const [row] = env.hub.metricsHistory(30, 20).recent.rooms;
  assert.deepEqual(Object.keys(row).sort(), ['capacity', 'duration_ms', 'ended', 'peak', 'platforms', 'state', 'transport']);
  assert.deepEqual([row.capacity, row.peak, row.transport, row.platforms, row.state, row.duration_ms],
    [4, 3, 'mixed', 'Linux', 'Ended', 5 * 60 * 1000]);
  const text = JSON.stringify(env.host.sql.all('SELECT * FROM recent_rooms'));
  for (const secret of [room, host.who.player, guests[0].who.player, '127.0.0.1']) assert.ok(!text.includes(secret));
});

test('the final state says only what the service can tell', () => {
  const env = makeHub();
  const closed = (first, act) => {
    const p = party(env, 2, 2, { first });
    act(p);
    return env.hub.metricsHistory(30, 20).recent.rooms[0].state;
  };
  assert.equal(closed(10, (p) => p.host.call('room.leave', { room: p.room })), 'Not started');
  assert.equal(closed(20, (p) => {
    p.host.call('room.start', { room: p.room });
    p.host.call('room.leave', { room: p.room });
  }), 'Failed');
  assert.equal(closed(30, (p) => {
    p.host.call('room.start', { room: p.room });
    for (const c of p.clients) c.call('match.ready', { room: p.room, ready: true });
    p.host.call('room.leave', { room: p.room });
  }), 'Ended');
  assert.equal(closed(40, (p) => {
    p.host.drop();
    env.advance(31 * 1000);
  }), 'Disconnected');
  assert.equal(closed(50, (p) => {
    p.host.call('room.start', { room: p.room });
    env.advance(5 * 3600 * 1000);
  }), 'Timeout');
});

test('lone rooms stay out of the recent list, which is bounded by count and age', () => {
  const env = makeHub();
  const alone = greeted(env, 1);
  alone.call('room.cancel', { room: alone.call('room.host', {}).room.room });
  assert.equal(env.hub.metricsHistory(30, 20).recent.total, 0);
  for (let i = 0; i < RECENT_ROOMS_MAX + 10; i++) {
    env.hub.metrics.recordRoom({ duration_ms: 1, capacity: 2, peak: 2, transport: 'direct', platforms: 'Windows', state: 'Ended' });
  }
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM recent_rooms').n, RECENT_ROOMS_MAX);
  env.clock.now += (RECENT_ROOMS_DAYS + 1) * DAY;
  env.hub.metrics.trimRecent(env.clock.now);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM recent_rooms').n, 0);
});

test('samples are written when counts grow, and an alarm samples while anyone is connected', () => {
  const env = makeHub();
  greeted(env, 1);
  assert.equal(env.host.sql.all('SELECT * FROM metrics_samples').length, 1);
  assert.ok(env.host.alarmAt !== null);
  env.advance(16 * 60 * 1000);
  assert.ok(env.host.alarmAt === null || env.host.alarmAt > env.clock.now);
  greeted(env, 2);
  assert.equal(env.host.sql.all('SELECT * FROM metrics_samples').length, 2);
});

test('old samples go, daily counters stay', () => {
  const env = makeHub();
  greeted(env, 1);
  env.clock.now += (SAMPLE_DAYS + 1) * DAY;
  greeted(env, 2);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_samples').n, 1);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM metrics_daily').n, 2);
});

test('peaks grow with the day and writes happen only when they do', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  greeted(env, 2);
  a.drop();
  const writes = countWrites(env);
  const again = env.connect(identity(1));
  again.hello();
  assert.deepEqual(writesTo(writes, 'peak_online'), []);
  assert.equal(env.hub.metricsSummary().today.peak_online, 2);
});

test('rejects are written once per window, not on every reconnect', () => {
  const env = makeHub({ local: false });
  const writes = countWrites(env);
  const ip = '203.0.113.9';
  for (let i = 0; i < 5; i++) assert.equal(env.connect(identity(1), { ip }).hello({ proto: 2 }).code, 'proto');
  assert.equal(env.hub.metricsSummary().today.incompatible_rejects, 1);
  assert.equal(writesTo(writes, 'metrics').length, 2);
  env.clock.now += PROTECTION.rejectWindowMs;
  assert.equal(env.connect(identity(1), { ip }).hello({ proto: 2 }).code, 'proto');
  assert.equal(env.hub.metricsSummary().today.incompatible_rejects, 2);
});

test('an incompatible join or invite counts as a reject; a compatible one does not', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2, { build: 2 });
  befriend(a, b);
  a.call('room.host', {});
  assert.equal(a.call('invite.send', { player: b.who.player }).code, 'version_mismatch');
  assert.equal(env.hub.metricsSummary().today.incompatible_rejects, 1);
  assert.deepEqual(dimension(env, 'reject'), { version_mismatch: 1 });
});

test('history is bounded, reports every dimension and compares with the previous period', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('room.cancel', { room: a.call('room.host', {}).room.room });
  env.clock.now += 7 * DAY;
  const b = greeted(env, 2);
  b.call('room.cancel', { room: b.call('room.host', {}).room.room });
  env.clock.now += 1000;
  b.call('room.cancel', { room: b.call('room.host', {}).room.room });
  const history = env.hub.metricsHistory(7, 20);
  assert.equal(history.daily.length, 7);
  assert.deepEqual(Object.keys(history.dimensions), DIMENSIONS);
  assert.deepEqual([history.totals.rooms_created, history.previous.rooms_created], [2, 1]);
  assert.deepEqual(history.comparison.rooms_created, { current: 2, previous: 1, change: 100 });
  assert.equal(history.comparison.joins.change, null);
  for (const days of [0, HISTORY_MAX_DAYS + 1, 1.5]) assert.throws(() => env.hub.metricsHistory(days, 20), RangeError);
  for (const recent of [0, RECENT_ROOMS_MAX + 1]) assert.throws(() => env.hub.metricsHistory(7, recent), RangeError);
  assert.equal(env.hub.metricsHistory(HISTORY_MAX_DAYS, RECENT_ROOMS_MAX).daily.length, HISTORY_MAX_DAYS);
});

test('live numbers describe rooms, seats and transports as they are', () => {
  const env = makeHub();
  const big = party(env, 3, 4, { first: 10 });
  party(env, 2, 2, { first: 20 });
  const small = party(env, 2, 3, { first: 30 });
  big.host.call('match.transport', { room: big.room, seat: 1, transport: 'direct' });
  big.host.call('match.transport', { room: big.room, seat: 2, transport: 'relay' });
  small.host.call('room.start', { room: small.room });
  const { live } = env.hub.metricsSummary();
  assert.deepEqual([live.online, live.in_rooms, live.rooms, live.advertised, live.active], [7, 7, 3, 2, 1]);
  assert.deepEqual([live.rooms_2, live.rooms_3, live.rooms_4], [1, 1, 1]);
  assert.deepEqual([live.occupied_seats, live.max_seats, live.average_occupancy], [7, 9, 0.778]);
  assert.deepEqual([live.direct, live.relay, live.pending], [1, 1, 2]);
  assert.deepEqual(live.transport, [{ value: 'pending', count: 2 }, { value: 'direct', count: 1 }, { value: 'relay', count: 1 }]);
  const row = live.list.find((r) => r.capacity === 4);
  assert.deepEqual([row.state, row.occupied, row.connected, row.direct, row.relay, row.pending, row.relay_available],
    ['open', 3, 3, 1, 1, 0, true]);
});

test('a relay that closes puts its seats back to Pending, and a player with two connections counts once', () => {
  const env = makeHub();
  const { host, room, clients } = party(env, 2);
  host.call('match.transport', { room, seat: 1, transport: 'relay' });
  env.hub.endRelay(room, 'rate_limit');
  assert.deepEqual([env.hub.metricsSummary().live.relay, env.hub.metricsSummary().live.pending], [0, 1]);
  env.connect(clients[0].who).hello();
  const { live } = env.hub.metricsSummary();
  assert.deepEqual([live.online, live.connections], [2, 3]);
});

test('metrics never fail a hub operation', () => {
  const env = makeHub();
  env.hub.metrics.count = () => { throw new Error('metrics down'); };
  env.hub.metrics.observe = () => { throw new Error('metrics down'); };
  env.hub.metrics.session = () => { throw new Error('metrics down'); };
  const a = env.connect(identity(1));
  assert.equal(a.hello().code, 'ok');
  assert.equal(a.call('room.host', {}).code, 'ok');
  assert.ok(env.logs.some((l) => l.includes('metrics error')));
});

test('no metric table holds a name, code, address, token or room id', () => {
  const env = makeHub({ local: false });
  const ip = '203.0.113.200';
  const a = greeted(env, 1, { ip, country: 'BR', name: 'Quico' });
  const b = greeted(env, 2, { ip: '198.51.100.200', name: 'Ñoño' });
  befriend(a, b);
  const hosted = a.call('room.host', { capacity: 2 });
  joinRoom(a, b);
  a.call('match.transport', { room: hosted.room.room, seat: 1, transport: 'relay' });
  env.advance(SAMPLE_MS);
  a.call('room.leave', { room: hosted.room.room });
  env.advance(SAMPLE_MS);
  const text = JSON.stringify(['metrics_daily', 'metrics_daily_dimensions', 'metrics_samples', 'recent_rooms']
    .map((t) => env.host.sql.all(`SELECT * FROM ${t}`)));
  for (const secret of [a.who.player, b.who.player, a.who.token, ip, 'Quico', hosted.room.room, String(hosted.relay_token)]) {
    assert.ok(!text.includes(secret), secret);
  }
  assert.ok(text.includes('"BR"'));
});
