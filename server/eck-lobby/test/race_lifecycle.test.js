// Race outcomes across room.reopen: each race of a room ends once, as complete or as abandoned.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { metricsDay } from '../src/metrics.js';
import { makeHub, racingParty } from './harness.js';

const CHARACTERS = [6, 1];
const RACE = { competition: 'time_attack', track: 4, players: 2, characters: CHARACTERS };

const today = (env) => env.hub.metricsSummary().today;
const outcomes = (env) => [today(env).races_started, today(env).races_completed, today(env).races_abandoned];
const rows = (env, sql) => env.host.sql.all(sql, metricsDay(env.clock.now));
const trackRow = (env) => rows(env, `SELECT races, completed, abandoned, duration_ms FROM metrics_daily_race_outcomes
  WHERE day = ? AND dimension = 'track' AND value = '4'`)[0];
const competitionRow = (env) => rows(env, `SELECT races, completed, abandoned, duration_ms FROM metrics_daily_race_outcomes
  WHERE day = ? AND dimension = 'competition' AND value = 'time_attack'`)[0];
const characterCompleted = (env) => Object.fromEntries(
  rows(env, 'SELECT character, completed FROM metrics_daily_characters WHERE day = ?').map((r) => [r.character, r.completed]));
const finishes = (env) => rows(env,
  'SELECT character, position, count FROM metrics_daily_finish_positions WHERE day = ? ORDER BY character');

const start = (p, race) => assert.equal(p.host.call('race.start', { room: p.room, race, ...RACE }).code, 'ok');
const complete = (p, race, durationMs = 60_000) =>
  p.host.call('race.complete', { room: p.room, race, duration_ms: durationMs, places: [2, 1] }).code;

function reopenAndRace(p) {
  assert.equal(p.host.call('room.reopen', { room: p.room }).code, 'ok');
  assert.equal(p.host.call('room.start', { room: p.room }).code, 'ok');
  for (const client of p.clients) assert.equal(client.call('match.ready', { room: p.room, ready: true }).code, 'ok');
}

test('a completed race stays complete through room.reopen and the room closing', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  start(p, 0);
  env.advance(60_000);
  assert.equal(complete(p, 0), 'ok');
  assert.equal(p.host.call('room.reopen', { room: p.room }).code, 'ok');
  assert.deepEqual(outcomes(env), [1, 1, 0]);
  assert.equal(p.host.call('room.leave', { room: p.room }).code, 'ok');
  assert.deepEqual(outcomes(env), [1, 1, 0], 'going back to the Main Menu after the finish abandons nothing');
});

test('every race of one reopened room counts once and on its own', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  for (let race = 0; race < 3; race++) {
    if (race > 0) reopenAndRace(p);
    start(p, race);
    env.advance(60_000);
    assert.equal(complete(p, race), 'ok');
  }
  assert.deepEqual(outcomes(env), [3, 3, 0]);
  assert.deepEqual(trackRow(env), { races: 3, completed: 3, abandoned: 0, duration_ms: 180_000 });
  assert.equal(today(env).race_duration_ms, 180_000);
});

test('a repeated race.complete, before or after room.reopen, changes no metric', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  start(p, 0);
  env.advance(60_000);
  assert.equal(complete(p, 0), 'ok');
  assert.equal(complete(p, 0), 'ok');
  assert.equal(p.host.call('room.reopen', { room: p.room }).code, 'ok');
  assert.equal(complete(p, 0), 'ok');

  assert.deepEqual(outcomes(env), [1, 1, 0]);
  assert.equal(today(env).race_duration_ms, 60_000);
  assert.deepEqual(trackRow(env), { races: 1, completed: 1, abandoned: 0, duration_ms: 60_000 });
  assert.deepEqual(competitionRow(env), { races: 1, completed: 1, abandoned: 0, duration_ms: 60_000 });
  assert.deepEqual(characterCompleted(env), { 1: 1, 6: 1 });
  assert.deepEqual(finishes(env), [{ character: 1, position: 1, count: 1 }, { character: 6, position: 2, count: 1 }]);
});

test('a race left running by the host, then a reopened room, is abandoned once and does not touch the next race', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  start(p, 0);
  env.advance(1_000);
  reopenAndRace(p);
  assert.deepEqual(outcomes(env), [1, 0, 0], 'still pending: only the next race or the room closing ends it');
  start(p, 1);
  assert.deepEqual(outcomes(env), [2, 0, 1]);
  env.advance(60_000);
  assert.equal(complete(p, 1), 'ok');
  assert.deepEqual(outcomes(env), [2, 1, 1]);
  assert.equal(p.host.call('room.leave', { room: p.room }).code, 'ok');
  assert.deepEqual(outcomes(env), [2, 1, 1]);
  assert.deepEqual(trackRow(env), { races: 2, completed: 1, abandoned: 1, duration_ms: 60_000 });
  assert.deepEqual(characterCompleted(env), { 1: 1, 6: 1 }, 'the abandoned race adds no finish');
  assert.deepEqual(finishes(env).map((f) => f.count), [1, 1]);
});

test('the host leaving during a race abandons it, also from a reopened room', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  start(p, 0);
  env.advance(1_000);
  assert.equal(p.host.call('room.reopen', { room: p.room }).code, 'ok');
  assert.equal(p.host.call('room.leave', { room: p.room }).code, 'ok');
  assert.deepEqual(outcomes(env), [1, 0, 1]);
  assert.deepEqual(finishes(env), []);
});

test('a guest leaving a running race completes nothing, and the host can still finish it', () => {
  const env = makeHub();
  const p = racingParty(env, 3);
  p.host.call('race.start', { room: p.room, race: 0, competition: 'coins' });
  assert.equal(p.guests[0].call('room.leave', { room: p.room }).code, 'ok');
  assert.deepEqual(outcomes(env), [1, 0, 0]);
  env.advance(60_000);
  assert.equal(p.host.call('race.complete', { room: p.room, race: 0, duration_ms: 60_000 }).code, 'ok');
  assert.deepEqual(outcomes(env), [1, 1, 0]);
});

test('the host connection lost before the finish abandons the race once', () => {
  const env = makeHub();
  const p = racingParty(env, 2);
  start(p, 0);
  p.host.drop();
  assert.deepEqual(outcomes(env), [1, 0, 0], 'the seat is held for a resume');
  env.advance(31_000);
  assert.deepEqual(outcomes(env), [1, 0, 1]);
  env.advance(31_000);
  assert.deepEqual(outcomes(env), [1, 0, 1]);
});
