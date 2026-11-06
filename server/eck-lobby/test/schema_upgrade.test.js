// Starts from the tables as production had them before this feature: a Durable Object that exists is only ever added to.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { sqliteStore } from '../local/host.js';
import { Hub } from '../src/hub.js';

const OLD_ROOMS = `CREATE TABLE rooms (id TEXT PRIMARY KEY, host TEXT NOT NULL UNIQUE, capacity INTEGER NOT NULL,
  image TEXT NOT NULL, build INTEGER NOT NULL, rules TEXT NOT NULL, state TEXT NOT NULL,
  relay_token INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL, updated INTEGER NOT NULL, refreshed INTEGER NOT NULL,
  started INTEGER, established INTEGER, peak INTEGER NOT NULL DEFAULT 1, platforms TEXT NOT NULL DEFAULT '',
  used_direct INTEGER NOT NULL DEFAULT 0, used_relay INTEGER NOT NULL DEFAULT 0)`;

const OLD_METRICS_DAILY = `CREATE TABLE metrics_daily (day TEXT PRIMARY KEY,
  sessions INTEGER NOT NULL DEFAULT 0, rooms_created INTEGER NOT NULL DEFAULT 0, rooms_started INTEGER NOT NULL DEFAULT 0,
  joins INTEGER NOT NULL DEFAULT 0, invites INTEGER NOT NULL DEFAULT 0, incompatible_rejects INTEGER NOT NULL DEFAULT 0,
  direct_selected INTEGER NOT NULL DEFAULT 0, relay_selected INTEGER NOT NULL DEFAULT 0, races_started INTEGER NOT NULL DEFAULT 0,
  races_completed INTEGER NOT NULL DEFAULT 0, rooms_closed INTEGER NOT NULL DEFAULT 0, rooms_with_guest INTEGER NOT NULL DEFAULT 0,
  rooms_direct INTEGER NOT NULL DEFAULT 0, rooms_relay INTEGER NOT NULL DEFAULT 0, rooms_pending INTEGER NOT NULL DEFAULT 0,
  peak_online INTEGER NOT NULL DEFAULT 0, peak_in_rooms INTEGER NOT NULL DEFAULT 0, peak_rooms INTEGER NOT NULL DEFAULT 0)`;

function previousProductionStore() {
  const sql = sqliteStore();
  sql.run(OLD_ROOMS);
  sql.run(OLD_METRICS_DAILY);
  sql.run(`CREATE TABLE players (player TEXT PRIMARY KEY, token_hash TEXT NOT NULL, name TEXT NOT NULL,
    created INTEGER NOT NULL, seen INTEGER NOT NULL, session INTEGER NOT NULL DEFAULT 0)`);
  sql.run(`CREATE TABLE friends (player TEXT NOT NULL, friend TEXT NOT NULL, since INTEGER NOT NULL, PRIMARY KEY (player, friend))`);
  sql.run('INSERT INTO players (player, token_hash, name, created, seen) VALUES (?, ?, ?, ?, ?)', 'ABCD-2345', 'hash', 'Saulo', 1, 1);
  sql.run('INSERT INTO friends (player, friend, since) VALUES (?, ?, ?)', 'ABCD-2345', 'WXYZ-9876', 1);
  sql.run(`INSERT INTO rooms (id, host, capacity, image, build, rules, state, created, updated, refreshed, peak, used_direct)
    VALUES ('deadbeef00000000', 'ABCD-2345', 2, 'eck-usa-1db4e96b', 1, '{}', 'open', 1, 1, 1, 2, 1)`);
  sql.run(`INSERT INTO metrics_daily (day, sessions, rooms_created, races_started) VALUES ('2026-10-01', 5, 2, 0)`);
  return sql;
}

test('a build that already wrote player counts keeps them and gains the transport columns', () => {
  const sql = previousProductionStore();
  sql.run(`CREATE TABLE metrics_daily_player_counts (day TEXT NOT NULL, player_count INTEGER NOT NULL, races INTEGER NOT NULL DEFAULT 0,
    completed INTEGER NOT NULL DEFAULT 0, abandoned INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (day, player_count))`);
  sql.run(`INSERT INTO metrics_daily_player_counts (day, player_count, races, completed) VALUES ('2026-10-01', 2, 7, 5)`);
  new Hub(fakeHost(sql), { local: true });
  const row = sql.get('SELECT * FROM metrics_daily_player_counts');
  assert.deepEqual([row.races, row.completed, row.direct, row.relay, row.mixed], [7, 5, 0, 0, 0]);
});

function fakeHost(sql) {
  return { sql, conns: new Set(), log: () => {}, now: () => 2_000_000_000_000, sockets: () => [], setAlarm: () => {}, alarmAt: null };
}

test('the hub upgrades a previous-production schema in place, destroying nothing', () => {
  const sql = previousProductionStore();
  const host = fakeHost(sql);
  new Hub(host, { local: true });

  assert.deepEqual(sql.get('SELECT player, name FROM players WHERE player = ?', 'ABCD-2345'), { player: 'ABCD-2345', name: 'Saulo' });
  assert.equal(sql.get('SELECT COUNT(*) AS n FROM friends').n, 1);

  const room = sql.get('SELECT * FROM rooms WHERE id = ?', 'deadbeef00000000');
  assert.equal(room.peak, 2);
  assert.deepEqual([room.races, room.pending_race, room.pending_started, room.pending_players, room.pending_competition,
    room.pending_track, room.pending_characters], [0, null, null, null, null, null, null]);

  const day = sql.get(`SELECT * FROM metrics_daily WHERE day = '2026-10-01'`);
  assert.deepEqual([day.sessions, day.rooms_created], [5, 2]);
  assert.deepEqual([day.races_abandoned, day.rooms_with_race, day.rooms_rematched, day.race_duration_ms, day.races_in_rooms],
    [0, 0, 0, 0, 0]);

  assert.equal(sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
  assert.equal(sql.get('SELECT COUNT(*) AS n FROM metrics_daily').n, 1);
});

test('the hub works and the dashboard loads against an upgraded schema, and upgrading twice is harmless', () => {
  const sql = previousProductionStore();
  const host = fakeHost(sql);
  const hub = new Hub(host, { local: true });

  assert.doesNotThrow(() => hub.metricsSummary());
  assert.doesNotThrow(() => hub.metricsHistory(30, 20));
  const history = hub.metricsHistory(30, 20);
  assert.equal(history.totals.races_abandoned, 0);
  assert.deepEqual([history.totals.unique_players, history.players_first_day, hub.metricsSummary().today.unique_players], [0, null, 0]);
  assert.deepEqual(history.party_sizes.map((s) => s.players), [1, 2, 3, 4]);
  assert.equal(history.dimensions.player_count.length, 0);
  assert.deepEqual([history.characters, history.pairs, history.tracks, history.compositions[2]], [[], [], [], []]);

  assert.doesNotThrow(() => new Hub(host, { local: true }));
  assert.equal(sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
  assert.equal(sql.get('SELECT COUNT(*) AS n FROM metrics_daily').n, 1);
});
