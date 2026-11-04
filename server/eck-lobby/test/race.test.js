import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { MAX_RACES_PER_ROOM } from '../src/protocol.js';
import { metricsDay } from '../src/metrics.js';
import { greeted, makeHub, party, racingParty } from './harness.js';

const dimension = (env, name) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', metricsDay(env.clock.now), name)
  .map((r) => [r.value, r.count]));
const today = (env) => env.hub.metricsSummary().today;

test('a valid online race.start and race.complete count once each, with the real player count and competition', () => {
  const env = makeHub();
  const p = racingParty(env, 3, 4);
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'survival' }).code, 'ok');
  env.advance(90 * 1000);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 90 * 1000 }).code, 'ok');
  assert.deepEqual([today(env).races_started, today(env).races_completed, today(env).races_abandoned], [1, 1, 0]);
  assert.deepEqual(dimension(env, 'player_count'), { 3: 1 });
  assert.deepEqual(dimension(env, 'competition'), { survival: 1 });
  assert.deepEqual(dimension(env, 'race_duration_bucket'), { under_2m: 1 });
});

test('a resent race.start or race.complete is a harmless no-op, not a second race', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  for (let i = 0; i < 3; i++) assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'endurance' }).code, 'ok');
  assert.equal(today(env).races_started, 1);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 5000 }).code, 'ok');
  for (let i = 0; i < 3; i++) assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 5000 }).code, 'ok');
  assert.equal(today(env).races_completed, 1);
});

test('a race left running counts as abandoned when the room closes', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  p.host.call('room.leave', { room: p.room });
  assert.deepEqual([today(env).races_started, today(env).races_completed, today(env).races_abandoned], [1, 0, 1]);
  assert.deepEqual(dimension(env, 'player_count'), { 2: 1 });
});

test('a race left running is abandoned when superseded by the next race.start, not lost', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  assert.equal(p.host.call('race.start', { room: p.room, race: 1, competition: 'coins' }).code, 'ok');
  assert.deepEqual([today(env).races_started, today(env).races_abandoned], [2, 1]);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 1, duration_ms: 1000 }).code, 'ok');
  assert.deepEqual([today(env).races_completed, today(env).races_abandoned], [1, 1]);
});

test('1, 2, 3 and 4 human players are each reported as the room\'s real occupied seat count', () => {
  const env = makeHub();
  let first = 10;
  for (const size of [2, 3, 4]) {
    const p = racingParty(env, size, size, { first });
    first += 10;
    assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'normal_race' }).code, 'ok');
    assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 }).code, 'ok');
  }
  const solo = greeted(env, 90);
  assert.equal(solo.call('race.start', { race: 0, competition: 'normal_race' }).code, 'ok');
  assert.equal(solo.call('race.complete', { race: 0, duration_ms: 1000 }).code, 'ok');
  assert.deepEqual(dimension(env, 'player_count'), { 1: 1, 2: 1, 3: 1, 4: 1 });
});

test('a local race needs no room, dedupes, supersedes, and a counter ahead of the last one is simply the next race', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.call('race.start', { race: 0, competition: 'time_attack' }).code, 'ok');
  assert.equal(a.call('race.start', { race: 0, competition: 'time_attack' }).code, 'ok');
  assert.equal(today(env).races_started, 1);
  assert.equal(a.call('race.start', { race: 7, competition: 'time_attack' }).code, 'ok');
  assert.deepEqual([today(env).races_started, today(env).races_abandoned], [2, 1]);
  assert.equal(a.call('race.complete', { race: 7, duration_ms: 1000 }).code, 'ok');
  assert.equal(a.call('race.complete', { race: 7, duration_ms: 1000 }).code, 'ok');
  assert.deepEqual([today(env).races_completed, today(env).races_abandoned], [1, 1]);
});

test('a reconnected client keeps racing: its counter does not have to restart at 0', () => {
  const env = makeHub();
  const before = greeted(env, 1);
  before.call('race.start', { race: 3, competition: 'coins' });
  before.call('race.complete', { race: 3, duration_ms: 1000 });
  before.drop();
  const after = greeted(env, 1);
  assert.equal(after.call('race.start', { race: 4, competition: 'coins' }).code, 'ok');
  assert.equal(today(env).races_started, 2);
});

test('a local race whose connection ends is abandoned, and only once', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('race.start', { race: 0, competition: 'coins' });
  a.drop();
  a.hub.closed(a.conn);
  assert.deepEqual([today(env).races_started, today(env).races_abandoned], [1, 1]);
});

test('split-screen humans on one PC are one local race of that many players, with no transport', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.call('race.start', { race: 0, competition: 'normal_race', players: 3, characters: [0, 1, 2] }).code, 'ok');
  assert.equal(a.call('race.complete', { race: 0, duration_ms: 2000 }).code, 'ok');
  assert.deepEqual(dimension(env, 'player_count'), { 3: 1 });
  assert.deepEqual(dimension(env, 'race_context'), { local: 1 });
  assert.deepEqual(dimension(env, 'race_transport'), {});
});

test('impossible player counts and mismatched rosters are refused', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.call('race.start', { race: 0, competition: 'coins', players: 5 }).code, 'bad_request');
  assert.equal(a.call('race.start', { race: 0, competition: 'coins', players: 0 }).code, 'bad_request');
  assert.equal(a.call('race.start', { race: 0, competition: 'coins', players: 2, characters: [0] }).code, 'bad_request');
  assert.equal(a.call('race.start', { race: 0, competition: 'coins', characters: [0, 1, 2, 3, 4] }).code, 'bad_request');
  const p = racingParty(env, 2, 2, { first: 10 });
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'coins', players: 3 }).code, 'bad_request');
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'coins', characters: [0, 1, 2] }).code, 'bad_request');
  assert.equal(today(env).races_started, 0);
});

test('character and track ids must be real game ids, never strings or anything out of range', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  for (const characters of [[12], [-1], ['chaves'], [1.5], []]) {
    assert.equal(a.call('race.start', { race: 0, competition: 'coins', characters }).code, 'bad_request', JSON.stringify(characters));
  }
  for (const track of [20, -1, 'PistaCiudad_Circuito0', 1.5]) {
    assert.equal(a.call('race.start', { race: 0, competition: 'coins', track }).code, 'bad_request', String(track));
  }
  assert.equal(a.call('race.start', { race: 0, competition: 'coins', characters: [11], track: 19 }).code, 'ok');
});

test('places must fit the race: one distinct place per human', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  a.call('race.start', { race: 0, competition: 'coins', players: 2, characters: [0, 1] });
  for (const places of [[1], [1, 1], [0, 2], [1, 2, 3], [1, 99], 'first']) {
    assert.equal(a.call('race.complete', { race: 0, duration_ms: 1000, places }).code, 'bad_request', JSON.stringify(places));
  }
  assert.equal(a.call('race.complete', { race: 0, duration_ms: 1000, places: [3, 1] }).code, 'ok');
});

test('a room stops counting at its race limit', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  for (let race = 0; race < MAX_RACES_PER_ROOM; race++) {
    env.clock.now += 1000;
    assert.equal(p.host.call('race.start', { room: p.room, race, competition: 'coins' }).code, 'ok');
  }
  assert.equal(p.host.call('race.start', { room: p.room, race: MAX_RACES_PER_ROOM, competition: 'coins' }).code, 'limit');
});

test('an unknown competition is rejected', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'grand_prix' }).code, 'bad_request');
  assert.equal(today(env).races_started, 0);
});

test('duration is bad_request out of 1s..1h, and is otherwise clamped to what the service itself measured', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 500 }).code, 'bad_request');
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 2 * 3600 * 1000 }).code, 'bad_request');
  env.advance(10 * 1000);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 50 * 60 * 1000 }).code, 'ok');
  assert.deepEqual(dimension(env, 'race_duration_bucket'), { under_2m: 1 });
});

test('only the host may report an online race', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  assert.equal(p.guests[0].call('race.start', { room: p.room, race: 0, competition: 'coins' }).code, 'not_host');
});

test('a race cannot be reported before the room is racing', () => {
  const env = makeHub();
  const p = party(env, 2);
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' }).code, 'race_not_ready');
});

test('a race.complete that does not match the pending race is race_mismatch', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 }).code, 'race_mismatch');
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  assert.equal(p.host.call('race.complete', { room: p.room, race: 1, duration_ms: 1000 }).code, 'race_mismatch');
});

test('race telemetry has its own rate limit, generous enough for ordinary rematches', () => {
  const env = makeHub({ local: false });
  const p = racingParty(env, 2);
  let limited = 0;
  for (let i = 0; i < PROTECTION.raceRoom.burst + 5; i++) {
    const code = p.host.call('race.start', { room: p.room, race: Math.min(i, MAX_RACES_PER_ROOM - 1), competition: 'coins' }).code;
    if (code === 'limit') limited++;
  }
  assert.ok(limited > 0);
});

test('races per room are counted at close: 1, 2, 3 and 4-or-more races, and a rematch is 2 or more', () => {
  const env = makeHub();
  for (const races of [1, 2, 4]) {
    const p = racingParty(env, 2, 2, { first: races * 10 });
    for (let i = 0; i < races; i++) {
      p.host.call('race.start', { room: p.room, race: i, competition: 'coins' });
      p.host.call('race.complete', { room: p.room, race: i, duration_ms: 1000 });
    }
    p.host.call('room.leave', { room: p.room });
  }
  assert.deepEqual(dimension(env, 'races_per_room'), { 1: 1, 2: 1, '4+': 1 });
  assert.deepEqual([today(env).rooms_with_race, today(env).rooms_rematched], [3, 2]);
  const totals = env.hub.metricsHistory(7, 20).totals;
  assert.equal(totals.races_in_rooms, 7);
});

test('a room that never raced is not part of the races-per-room distribution', () => {
  const env = makeHub();
  const p = party(env, 2);
  p.host.call('room.leave', { room: p.room });
  assert.deepEqual(dimension(env, 'races_per_room'), {});
  assert.equal(today(env).rooms_with_race, 0);
});

test('the race transport is what the seats ride on as it ends, whatever the room used before', () => {
  const env = makeHub();
  const p = racingParty(env, 3, 4);
  p.host.call('match.transport', { room: p.room, seat: 1, transport: 'relay' });
  p.host.call('match.transport', { room: p.room, seat: 2, transport: 'relay' });
  p.host.call('match.transport', { room: p.room, seat: 1, transport: 'direct' });
  p.host.call('match.transport', { room: p.room, seat: 2, transport: 'direct' });
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 });
  assert.deepEqual(dimension(env, 'race_transport'), { direct: 1 });
});

test('direct, relay and mixed are counted by player count', () => {
  const env = makeHub();
  const race = (first, size, kinds) => {
    const p = racingParty(env, size, size, { first });
    kinds.forEach((transport, i) => p.host.call('match.transport', { room: p.room, seat: i + 1, transport }));
    p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
    p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 });
  };
  race(10, 2, ['direct']);
  race(20, 2, ['relay']);
  race(30, 3, ['direct', 'relay']);
  race(40, 4, ['relay', 'relay', 'relay']);
  const rows = Object.fromEntries(env.hub.metricsHistory(7, 20).player_counts.map((r) => [r.player_count, [r.direct, r.relay, r.mixed]]));
  assert.deepEqual(rows, { 2: [1, 1, 0], 3: [0, 0, 1], 4: [0, 1, 0] });
});

test('the race transport is the room\'s own already-tracked direct/relay/mixed', () => {
  const env = makeHub();
  const p = racingParty(env, 3, 4);
  p.host.call('match.transport', { room: p.room, seat: 1, transport: 'direct' });
  p.host.call('match.transport', { room: p.room, seat: 2, transport: 'relay' });
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 });
  assert.deepEqual(dimension(env, 'race_transport'), { mixed: 1 });
});

test('metrics never fail race.start or race.complete', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  env.hub.metrics.raceStarted = () => { throw new Error('metrics down'); };
  env.hub.metrics.raceCompleted = () => { throw new Error('metrics down'); };
  assert.equal(p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' }).code, 'ok');
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 }).code, 'ok');
  assert.ok(env.logs.some((l) => l.includes('metrics error')));
});

test('no race telemetry row holds a name, code, address, token or room id', () => {
  const env = makeHub({ local: false });
  const ip = '203.0.113.50';
  const p = racingParty(env, 2, 2, { options: { ip, name: 'Quico' } });
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 1000 });
  p.host.call('room.leave', { room: p.room });
  const text = JSON.stringify(env.host.sql.all('SELECT * FROM metrics_daily_dimensions'));
  for (const secret of [p.host.who.player, p.guests[0].who.player, p.host.who.token, ip, 'Quico', p.room]) {
    assert.ok(!text.includes(secret), secret);
  }
});
