import assert from 'node:assert/strict';
import { test } from 'node:test';

import { HISTORY_MAX_DAYS, PLAYER_RETENTION_DAYS, SAMPLE_MS, metricsDay } from '../src/metrics.js';
import { countWrites, greeted, identity, makeHub, party, writesTo } from './harness.js';

const DAY = 24 * 3600 * 1000;
const unique = (env, days = 30) => env.hub.metricsHistory(days, 1).totals.unique_players;
const today = (env) => env.hub.metricsSummary().today.unique_players;

test('a signed-in player counts once a day however often they reconnect', () => {
  const env = makeHub();
  const who = identity(1);
  for (let i = 0; i < 4; i++) {
    const client = env.connect(who);
    assert.equal(client.hello().code, 'ok');
    client.drop();
    env.advance(20 * 60 * 1000);
  }
  assert.equal(today(env), 1);
  assert.equal(unique(env), 1);
});

test('the connection test and refused sign-ins are not players', () => {
  const env = makeHub();
  env.connect(identity(1), { presence: false }).hello();
  assert.equal(env.connect(identity(2)).hello({ proto: 999 }).code, 'proto');
  env.connect(identity(3), { presence: false }).hello();
  assert.equal(env.connect(identity(3)).hello({ token: 'b'.repeat(64) }).code, 'unauthorized');
  assert.equal(today(env), 0);
});

test('a period counts each player once across its days and each day counts its own', () => {
  const env = makeHub();
  for (const n of [1, 2, 3]) greeted(env, n);
  env.clock.now += DAY;
  for (const n of [2, 3, 4]) greeted(env, n);
  env.clock.now += DAY;
  for (const n of [4, 5]) greeted(env, n);
  const history = env.hub.metricsHistory(7, 1);
  assert.deepEqual(history.daily.slice(-3).map((d) => d.unique_players), [3, 3, 2]);
  assert.equal(history.totals.unique_players, 5);
  assert.equal(today(env), 2);
  assert.equal(history.players_first_day, metricsDay(env.clock.now - 2 * DAY));
});

test('the previous period is the same number of days before, so a player is new or returning', () => {
  const env = makeHub();
  for (const n of [1, 2]) greeted(env, n);
  env.clock.now += 8 * DAY;
  for (const n of [2, 3, 4]) greeted(env, n);
  const { totals, previous, comparison } = env.hub.metricsHistory(7, 1);
  assert.deepEqual([totals.unique_players, previous.unique_players], [3, 2]);
  assert.deepEqual(comparison.unique_players, { current: 3, previous: 2, change: 50 });
});

test('a repeat sign-in writes nothing while the object stays awake', () => {
  const env = makeHub();
  greeted(env, 1);
  const writes = countWrites(env);
  for (let i = 0; i < 3; i++) env.connect(identity(1)).hello();
  assert.equal(writesTo(writes, 'metrics_players').length, 0);
});

test('only a keyed pseudonym is stored, never the friend code', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const rows = env.host.sql.all('SELECT * FROM metrics_players');
  assert.equal(rows.length, 2);
  assert.deepEqual(Object.keys(rows[0]).sort(), ['day', 'pseudonym']);
  for (const row of rows) {
    assert.match(row.pseudonym, /^[0-9a-f]{32}$/);
    assert.ok(![a.who.player, b.who.player, a.who.token].some((secret) => row.pseudonym.includes(secret)));
  }
  assert.notEqual(rows[0].pseudonym, rows[1].pseudonym);
  assert.ok(!JSON.stringify(env.hub.metricsHistory(30, 50)).includes(rows[0].pseudonym));
});

test('pseudonyms older than the longest range and its comparison period are removed', () => {
  const env = makeHub();
  greeted(env, 1);
  env.clock.now += (PLAYER_RETENTION_DAYS + 2) * DAY;
  greeted(env, 2);
  env.advance(SAMPLE_MS);
  env.hub.observe();
  const days = env.host.sql.all('SELECT day FROM metrics_players').map((r) => r.day);
  assert.deepEqual(days, [metricsDay(env.clock.now)]);
  assert.ok(PLAYER_RETENTION_DAYS >= 2 * HISTORY_MAX_DAYS);
});

test('the comparison reports races by player count and no percentage without an earlier period', () => {
  const env = makeHub();
  const solo = greeted(env, 1);
  solo.call('race.start', { race: 0, competition: 'normal_race', characters: [0, 1] });
  const { comparison } = env.hub.metricsHistory(7, 1);
  assert.deepEqual(comparison.races_2p, { current: 1, previous: 0, change: null });
  assert.deepEqual([comparison.races_3p.current, comparison.races_4p.current], [0, 0]);
  assert.equal(comparison.unique_players.change, null);
});

test('party size is the largest party a room reached, with rooms that never had a guest as solo', () => {
  const env = makeHub();
  const rooms = [[1, 2], [1, 3], [2, 2], [3, 4], [4, 4], [4, 4]];
  rooms.forEach(([size, capacity], i) => {
    const p = party(env, size, capacity, { first: 10 + i * 10 });
    p.host.call(size === 1 ? 'room.cancel' : 'room.leave', { room: p.room });
  });
  const open = party(env, 3, 3, { first: 100 });
  assert.ok(open.room);
  const history = env.hub.metricsHistory(30, 1);
  assert.deepEqual(history.party_sizes, [{ players: 1, count: 2 }, { players: 2, count: 1 }, { players: 3, count: 1 }, { players: 4, count: 2 }]);
  assert.equal(history.party_sizes.reduce((sum, s) => sum + s.count, 0), history.totals.rooms_closed);
});

test('race player counts come from race starts, not from room size', () => {
  const env = makeHub();
  const p = party(env, 4, 4, { first: 10 });
  const solo = greeted(env, 50);
  solo.call('race.start', { race: 0, competition: 'time_attack', characters: [3] });
  solo.call('race.start', { race: 1, competition: 'time_attack', characters: [3, 4] });
  const counts = Object.fromEntries(env.hub.metricsHistory(30, 1).player_counts.map((c) => [c.player_count, c.races]));
  assert.deepEqual(counts, { 1: 1, 2: 1 });
  assert.ok(p.room);
});
