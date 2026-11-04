import assert from 'node:assert/strict';
import { test } from 'node:test';

import { CHARACTERS, COMPETITIONS, TRACKS } from '../src/game_data.js';
import { countWrites, greeted, makeHub, racingParty } from './harness.js';

function localRace(env, first, characters, { places, track = 4, competition = 'normal_race', complete = true } = {}) {
  const client = greeted(env, first);
  client.call('race.start', { race: 0, competition, track, players: characters.length, characters });
  env.advance(60 * 1000);
  if (complete) client.call('race.complete', { race: 0, duration_ms: 60 * 1000, ...(places ? { places } : {}) });
  return client;
}

const history = (env) => env.hub.metricsHistory(7, 20);
const stat = (env, character) => history(env).characters.find((c) => c.character === character);

test('the game tables are the fixed sets the script declares', () => {
  assert.deepEqual([CHARACTERS.length, TRACKS.length, COMPETITIONS.length], [12, 20, 9]);
  CHARACTERS.forEach((c, i) => assert.equal(c.id, i));
  TRACKS.forEach((t, i) => assert.equal(t.id, i));
  assert.equal(new Set(TRACKS.map((t) => t.map)).size, 20);
  assert.ok(!COMPETITIONS.some((c) => ['podium', 'matador', 'matador_teams'].includes(c.id)));
});

test('character picks: seats, races containing the character, and completed', () => {
  const env = makeHub();
  localRace(env, 1, [0, 1]);
  localRace(env, 2, [0], { complete: false });
  const chaves = stat(env, 0);
  assert.deepEqual([chaves.picks, chaves.races, chaves.completed], [2, 2, 1]);
  assert.deepEqual([stat(env, 1).picks, stat(env, 1).completed], [1, 1]);
});

test('a character picked by two seats is two picks in one race', () => {
  const env = makeHub();
  localRace(env, 1, [0, 0, 1]);
  assert.deepEqual([stat(env, 0).picks, stat(env, 0).races, stat(env, 0).completed], [2, 1, 2]);
});

test('pairs: 2 players make 1, 3 make 3, 4 make 6, in any seat order', () => {
  const env = makeHub();
  localRace(env, 1, [1, 0]);
  assert.deepEqual(history(env).pairs, [{ a: 0, b: 1, races: 1 }]);
  localRace(env, 2, [0, 1]);
  assert.deepEqual(history(env).pairs, [{ a: 0, b: 1, races: 2 }]);

  const three = makeHub();
  localRace(three, 1, [5, 2, 9]);
  assert.deepEqual(history(three).pairs.map((p) => [p.a, p.b]), [[2, 5], [2, 9], [5, 9]]);

  const four = makeHub();
  localRace(four, 1, [3, 0, 2, 1]);
  assert.equal(history(four).pairs.length, 6);
  assert.equal(history(four).multiplayer_races, 1);
});

test('duplicate characters: each distinct pair counts once per race, and a self pair needs two seats', () => {
  const env = makeHub();
  localRace(env, 1, [0, 0, 1]);
  assert.deepEqual(history(env).pairs.map((p) => [p.a, p.b, p.races]), [[0, 0, 1], [0, 1, 1]]);
  const solo = makeHub();
  localRace(solo, 1, [0, 1]);
  assert.ok(!history(solo).pairs.some((p) => p.a === p.b));
});

test('a solo race adds no pair and no composition', () => {
  const env = makeHub();
  localRace(env, 1, [0]);
  assert.deepEqual(history(env).pairs, []);
  assert.equal(history(env).multiplayer_races, 0);
});

test('compositions are kept per player count, sorted, with multiplicity', () => {
  const env = makeHub();
  localRace(env, 1, [1, 0]);
  localRace(env, 2, [0, 1]);
  localRace(env, 3, [2, 0, 1]);
  localRace(env, 4, [3, 2, 1, 0]);
  localRace(env, 5, [1, 0, 0]);
  const { compositions } = history(env);
  assert.deepEqual(compositions[2], [{ characters: [0, 1], races: 2 }]);
  assert.deepEqual(compositions[3].map((c) => c.characters), [[0, 0, 1], [0, 1, 2]]);
  assert.deepEqual(compositions[4], [{ characters: [0, 1, 2, 3], races: 1 }]);
});

test('finish position: places become ranks among the humans, at 2, 3 and 4 players', () => {
  const two = makeHub();
  localRace(two, 1, [0, 1], { places: [7, 3] });
  assert.deepEqual([stat(two, 1).positions, stat(two, 0).positions], [[1, 0, 0, 0], [0, 1, 0, 0]]);

  const three = makeHub();
  localRace(three, 1, [0, 1, 2], { places: [2, 8, 5] });
  assert.deepEqual([0, 1, 2].map((c) => stat(three, c).positions), [[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0]]);

  const four = makeHub();
  localRace(four, 1, [0, 1, 2, 3], { places: [4, 3, 2, 1] });
  assert.deepEqual([0, 1, 2, 3].map((c) => stat(four, c).positions[3 - c]), [1, 1, 1, 1]);
});

test('average finish, wins, podiums and last places', () => {
  const env = makeHub();
  localRace(env, 1, [0, 1], { places: [1, 2] });
  localRace(env, 2, [0, 1, 2], { places: [3, 1, 2] });
  localRace(env, 3, [0, 1, 2, 3], { places: [2, 1, 3, 4] });
  const chaves = stat(env, 0);
  assert.deepEqual([chaves.ranked, chaves.wins, chaves.podiums, chaves.last_places, chaves.average_finish], [3, 1, 2, 1, 2]);
  const seu = stat(env, 2);
  assert.deepEqual([seu.ranked, seu.wins, seu.podiums, seu.last_places, seu.average_finish], [2, 0, 2, 0, 2.5]);
});

test('2 players have one podium place, 3 have two, 4 have three: last is never a podium', () => {
  const env = makeHub();
  localRace(env, 1, [0, 1], { places: [1, 2] });
  localRace(env, 2, [2, 3, 4], { places: [1, 2, 3] });
  localRace(env, 3, [5, 6, 7, 8], { places: [1, 2, 3, 4] });
  assert.deepEqual([0, 1, 2, 3, 4, 5, 6, 7, 8].map((c) => stat(env, c).podiums), [1, 0, 1, 1, 0, 1, 1, 1, 0]);
});

test('only completed races with places rank anyone', () => {
  const env = makeHub();
  localRace(env, 1, [0, 1], { complete: false });
  localRace(env, 2, [0, 1]);
  localRace(env, 3, [0], { places: [1] });
  assert.equal(stat(env, 0).ranked, 0);
  assert.equal(stat(env, 0).average_finish, null);
});

test('a race the client could not identify still counts everywhere else', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('race.start', { race: 0, competition: 'challenges' });
  env.advance(60 * 1000);
  a.call('race.complete', { race: 0, duration_ms: 60 * 1000 });
  const h = history(env);
  assert.deepEqual([h.totals.races_started, h.totals.races_completed], [1, 1]);
  assert.deepEqual(h.competitions.map((c) => [c.value, c.races, c.completed]), [['challenges', 1, 1]]);
  assert.deepEqual([h.tracks, h.characters], [[], []]);
});

test('track metrics: picks, completed, abandoned, duration, and breakdowns by players and competition', () => {
  const env = makeHub();
  localRace(env, 1, [0], { track: 7 });
  localRace(env, 2, [0, 1], { track: 7, competition: 'coins', complete: false });
  env.hub.closed(env.host.sockets()[1]);
  const h = history(env);
  assert.deepEqual(h.tracks.map((t) => [t.value, t.races, t.completed, t.abandoned, t.duration_ms]), [['7', 2, 1, 1, 60 * 1000]]);
  const pairs = (kind) => h.race_cross[kind].map((r) => [r.a, r.b, r.count]).sort();
  assert.deepEqual(pairs('track_players'), [['7', '1', 1], ['7', '2', 1]]);
  assert.deepEqual(pairs('track_competition'), [['7', 'coins', 1], ['7', 'normal_race', 1]]);
});

test('competition metrics: completion and duration, and player count by competition', () => {
  const env = makeHub();
  localRace(env, 1, [0], { competition: 'survival' });
  localRace(env, 2, [0, 1], { competition: 'survival' });
  localRace(env, 3, [2], { competition: 'coins' });
  const h = history(env);
  assert.deepEqual(h.competitions.map((c) => [c.value, c.races, c.completed]), [['survival', 2, 2], ['coins', 1, 1]]);
  assert.deepEqual(h.race_cross.competition_players.map((r) => [r.a, r.b, r.count]).sort(),
    [['coins', '1', 1], ['survival', '1', 1], ['survival', '2', 1]]);
});

test('character by competition and by track count picks', () => {
  const env = makeHub();
  localRace(env, 1, [0, 1], { competition: 'survival', track: 3 });
  localRace(env, 2, [0, 0], { competition: 'survival', track: 3 });
  const h = history(env);
  assert.deepEqual(h.race_cross.character_competition.map((r) => [r.a, r.b, r.count]), [['0', 'survival', 3], ['1', 'survival', 1]]);
  assert.deepEqual(h.race_cross.character_track.map((r) => [r.a, r.b, r.count]), [['0', '3', 3], ['1', '3', 1]]);
});

test('an online room reports the same analytics, once, from its host', () => {
  const env = makeHub();
  const p = racingParty(env, 3);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'normal_race', track: 1, characters: [0, 1, 2] });
  assert.equal(p.guests[0].call('race.start', { room: p.room, race: 0, competition: 'normal_race', track: 1, characters: [0, 1, 2] }).code,
    'not_host');
  env.advance(60 * 1000);
  p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 60 * 1000, places: [3, 1, 2] });
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 60 * 1000, places: [3, 1, 2] }).code, 'ok');
  const h = history(env);
  assert.deepEqual([h.totals.races_started, h.totals.races_completed], [1, 1]);
  assert.deepEqual([stat(env, 1).wins, stat(env, 0).last_places], [1, 1]);
  assert.deepEqual(h.dimensions.race_context, [{ value: 'online', count: 1 }]);
});

test('an abandoned online race keeps its track and competition on the abandoned side', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins', track: 2, characters: [0, 1] });
  p.host.call('room.leave', { room: p.room });
  const h = history(env);
  assert.deepEqual(h.tracks.map((t) => [t.races, t.completed, t.abandoned]), [[1, 0, 1]]);
  assert.deepEqual(h.competitions.map((c) => [c.races, c.completed, c.abandoned]), [[1, 0, 1]]);
  assert.equal(stat(env, 0).completed, 0);
});

test('previous-period picks sit beside the current ones', () => {
  const env = makeHub();
  localRace(env, 1, [0]);
  env.clock.now += 8 * 24 * 3600 * 1000;
  localRace(env, 2, [0]);
  localRace(env, 3, [0]);
  const chaves = history(env).characters.find((c) => c.character === 0);
  assert.deepEqual([chaves.picks, chaves.previous_picks], [2, 1]);
});

test('a 4-player race stays a small bounded number of writes, none per frame', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const writes = countWrites(env);
  a.call('race.start', { race: 0, competition: 'normal_race', track: 4, players: 4, characters: [0, 1, 2, 3] });
  const started = writes.length;
  env.advance(60 * 1000);
  writes.length = 0;
  a.call('race.complete', { race: 0, duration_ms: 60 * 1000, places: [1, 2, 3, 4] });
  assert.ok(started <= 12, `start wrote ${started}`);
  assert.ok(writes.filter((q) => q.includes('metrics')).length <= 10, `complete wrote ${writes.length}`);
});

function walk(value, seen = []) {
  if (value === null || typeof value !== 'object') seen.push(String(value));
  else for (const [key, inner] of Object.entries(value)) { seen.push(key); walk(inner, seen); }
  return seen;
}

test('privacy: no stored metric row and no dashboard field holds who played', () => {
  const env = makeHub({ local: false });
  const ip = '203.0.113.77';
  const p = racingParty(env, 2, 2, { options: { ip, name: 'Godinez' } });
  p.host.call('match.transport', { room: p.room, seat: 1, transport: 'relay' });
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins', track: 1, characters: [0, 1] });
  env.advance(60 * 1000);
  p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 60 * 1000, places: [1, 2] });
  const solo = greeted(env, 40, { ip, name: 'Godinez' });
  solo.call('race.start', { race: 0, competition: 'coins', track: 1, characters: [3] });
  const relayToken = String(p.token);
  p.host.call('room.leave', { room: p.room });
  const tables = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND (name LIKE 'metrics%' OR name = 'recent_rooms')`)
    .map((r) => r.name);
  assert.ok(tables.length >= 9);
  const stored = tables.map((t) => env.host.sql.all(`SELECT * FROM ${t}`));
  const hash = env.host.sql.get('SELECT token_hash FROM players LIMIT 1').token_hash;
  const secrets = [p.host.who.player, p.guests[0].who.player, solo.who.player, p.host.who.token, hash, ip, 'Godinez', p.room, relayToken];
  for (const output of [stored, env.hub.metricsHistory(30, 50), env.hub.metricsSummary().today]) {
    const text = walk(output);
    for (const secret of secrets) assert.ok(!text.some((value) => value.includes(secret)), secret);
  }
  const columns = tables.flatMap((t) => env.host.sql.all(`PRAGMA table_info(${t})`).map((c) => c.name));
  for (const banned of ['player', 'friend', 'room', 'ip', 'token', 'name']) assert.ok(!columns.includes(banned), banned);
});
