import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { CHARACTERS, COMPETITIONS, TRACKS } from '../src/game_data.js';
import { openDashboard } from './dom.js';
import { greeted, makeHub, party } from './harness.js';

const DAY = 24 * 3600 * 1000;
const open = (env, options = {}) => openDashboard({ summary: env.hub.metricsSummary(), history: env.hub.metricsHistory(30, 1), ...options });
const openRange = (env, days, options = {}) => open(env, { range: String(days), historyFor: () => env.hub.metricsHistory(days, 1), ...options });

function race(env, first, characters, { competition = 'normal_race', track = 4, finish = true } = {}) {
  const client = greeted(env, first);
  client.call('race.start', { race: 0, competition, track, players: characters.length, characters });
  if (finish) {
    env.advance(90 * 1000);
    client.call('race.complete', { race: 0, duration_ms: 90 * 1000 });
  }
  return client;
}

const bars = (page, id) => page.nodes.get(id).find((n) => n.className === 'row').map((row) => {
  const [count, percent] = row.children[2].textContent.split(' · ');
  return { name: row.children[0].textContent, count: Number(count.replace(/,/g, '')), percent };
});

const barLabels = (page, id) => page.nodes.get(id).find((n) => n.attrs.class === 'value-label').map((n) => n.textContent);
const compareRows = (page) => Object.fromEntries(page.rows('compare').map((r) => [r[0], r.slice(1)]));

const PLAYER_RACES = [[0], [1], [2], [0, 1], [1, 0], [2, 3], [4, 5], [0, 1, 2], [3, 4, 5], [0, 1, 2, 3]];

function playedPlayerRaces(env) {
  PLAYER_RACES.forEach((characters, i) => race(env, 10 + i, characters));
}

test('the sections follow the page order', () => {
  const order = ['id="range"', 'id="k-online"', 'Period comparison', 'Multiplayer activity', 'Players per race',
    '<h2 class="section">Characters</h2>', 'Character combinations', '<h2 class="section">Tracks</h2>', '<h2 class="section">Competitions</h2>',
    'Historical activity', 'Daily totals', '<details id="technical"'];
  let at = -1;
  for (const marker of order) {
    const next = ADMIN_HTML.indexOf(marker);
    assert.ok(next > at, marker);
    at = next;
  }
});

test('the top row answers who is playing now and in the period', async () => {
  const env = makeHub();
  const p = party(env, 3, 4, { first: 10 });
  const solo = party(env, 1, 2, { first: 20 });
  assert.ok(p.room && solo.room);
  race(env, 30, [0, 1], { finish: false });
  const page = await open(env);
  assert.deepEqual(['c-online', 'c-in-rooms', 'c-rooms', 'c-peak'].map(page.text), ['5', '4', '2', '5']);
  assert.deepEqual(['c-unique-today', 'c-unique-range', 'c-races-today', 'c-races-range'].map(page.text), ['5', '5', '1', '1']);
  assert.equal(page.text('c-unique-range-label'), 'Unique players · 30 days');
  assert.equal(page.text('c-races-range-label'), 'Races · 30 days');
});

test('the 7, 30, 90 and 365 day ranges each load their own history', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  for (const days of [7, 30, 90, 365]) {
    const page = await openRange(env, days);
    assert.ok(page.requests.includes(`/admin/api/history?days=${days}&recent=1`), String(days));
    assert.equal(page.text('c-unique-range-label'), `Unique players · ${days} days`);
    assert.equal(page.text('c-races-range'), '1');
    assert.match(page.text('compare-note'), new RegExp(`compared with .* to .*`));
  }
  assert.ok(ADMIN_HTML.includes('<option value="365">365 days</option>'));
});

test('unique players today and in the period count a player once however many days they came', async () => {
  const env = makeHub();
  for (const n of [1, 2, 3]) greeted(env, n);
  env.clock.now += DAY;
  for (const n of [2, 4]) greeted(env, n);
  const page = await openRange(env, 7);
  assert.equal(page.text('c-unique-today'), '2');
  assert.equal(page.text('c-unique-range'), '4');
  assert.deepEqual(barLabels(page, 'h-unique'), ['3', '2']);
  assert.deepEqual(compareRows(page)['Unique players'], ['4', '0', 'new']);
});

test('the unique player note says when counting started', async () => {
  assert.match((await open(makeHub())).text('cards-note'), /start counting at the first sign-in/);
  const env = makeHub();
  greeted(env, 1);
  const since = env.hub.metricsHistory(30, 1).players_first_day;
  assert.equal((await open(env)).text('cards-note'), `Unique players are counted from ${since}; earlier days show 0.`);
  env.clock.now += 10 * DAY;
  assert.equal((await openRange(env, 7)).text('cards-note'), '');
});

test('players per race shows 1, 2, 3 and 4 players with counts and percentages from race starts', async () => {
  const env = makeHub();
  playedPlayerRaces(env);
  const page = await open(env);
  assert.deepEqual(bars(page, 'd-player-count'), [
    { name: '1 player', count: 3, percent: '30%' },
    { name: '2 players', count: 4, percent: '40%' },
    { name: '3 players', count: 2, percent: '20%' },
    { name: '4 players', count: 1, percent: '10%' },
  ]);
  assert.match(page.text('d-race-context'), /Local/);
});

test('players per race keeps every bucket and ignores room occupancy', async () => {
  const env = makeHub();
  party(env, 4, 4, { first: 10 });
  race(env, 30, [0, 1]);
  const page = await open(env);
  assert.deepEqual(bars(page, 'd-player-count').map((b) => b.count), [0, 1, 0, 0]);
  assert.equal(bars(page, 'd-player-count')[1].percent, '100%');
});

test('party size separates solo rooms from 2, 3 and 4 players and does not count messages', async () => {
  const env = makeHub();
  const rooms = [[1, 2], [2, 4], [2, 3], [3, 3], [4, 4]];
  rooms.forEach(([size, capacity], i) => {
    const p = party(env, size, capacity, { first: 10 + i * 10 });
    if (size === 1) p.host.call('room.cancel', { room: p.room });
    else p.host.call('room.leave', { room: p.room });
  });
  const page = await open(env);
  assert.deepEqual(bars(page, 'd-party-size'), [
    { name: 'Solo (no guest joined)', count: 1, percent: '20%' },
    { name: '2 players', count: 2, percent: '40%' },
    { name: '3 players', count: 1, percent: '20%' },
    { name: '4 players', count: 1, percent: '20%' },
  ]);
  const cards = page.nodes.get('activity').find((n) => n.className === 'card').map((n) => n.textContent);
  assert.ok(cards.includes('Rooms created5'), cards.join('|'));
  assert.ok(cards.includes('Successful joins7'), cards.join('|'));
});

test('all 12 characters appear in game order with counts and percentages', async () => {
  const env = makeHub();
  playedPlayerRaces(env);
  const page = await open(env);
  const rows = bars(page, 'd-character');
  assert.deepEqual(rows.map((r) => r.name), CHARACTERS.map((c) => c.name));
  const picks = Array(12).fill(0);
  for (const characters of PLAYER_RACES) for (const c of characters) picks[c] += 1;
  assert.deepEqual(rows.map((r) => r.count), picks);
  assert.deepEqual(rows[0], { name: 'Chaves', count: 5, percent: '24%' });
  assert.deepEqual(rows[11], { name: 'Godines', count: 0, percent: '0%' });
  assert.equal(rows[9].name, 'Paty');
  assert.equal(rows.reduce((sum, r) => sum + r.count, 0), 21);
});

test('the roster keeps its order between ranges and zero rows stay visible', async () => {
  const env = makeHub();
  race(env, 10, [11, 11]);
  const page = await openRange(env, 7);
  const rows = bars(page, 'd-character');
  assert.equal(rows.length, 12);
  assert.equal(rows[11].count, 2);
  assert.equal(rows[0].name, 'Chaves');
});

test('a combination is one row whatever the seat order, with a Top 15 limit', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  race(env, 11, [1, 0]);
  race(env, 12, [9, 5, 1, 6]);
  const page = await open(env);
  const combos = bars(page, 'd-combination');
  assert.deepEqual(combos[0], { name: 'Chaves + Quico', count: 2, percent: '67%' });
  assert.deepEqual(combos[1], { name: 'Quico + Pópis + Nhonho + Paty', count: 1, percent: '33%' });
  assert.equal(combos.length, 2);

  const many = makeHub();
  let first = 100;
  for (let a = 0; a < 6; a++) for (let b = a + 1; b < 6; b++) race(many, first++, [a, b]);
  race(many, first++, [0, 1, 2]);
  race(many, first++, [3, 4, 5]);
  const full = await open(many);
  assert.equal(bars(full, 'd-combination').length, 15);
  assert.match(full.text('d-combination'), /2 more not shown/);
});

test('solo races are not character combinations', async () => {
  const env = makeHub();
  race(env, 10, [0]);
  const page = await open(env);
  assert.match(page.text('d-combination'), /No multiplayer races with known characters/);
});

test('exactly the 20 tracks appear, by name, with counts and percentages', async () => {
  const env = makeHub();
  TRACKS.forEach((t, i) => race(env, 10 + i, [0], { track: t.id }));
  race(env, 60, [0], { track: 4 });
  const page = await open(env);
  const rows = bars(page, 'd-track');
  assert.equal(rows.length, 20);
  assert.deepEqual(rows.map((r) => r.name).sort(), TRACKS.map((t) => t.name).sort());
  assert.deepEqual(rows[0], { name: 'Chavoltepec', count: 2, percent: '10%' });
  assert.ok(rows.slice(1).every((r) => r.count === 1 && r.percent === '5%'));
});

test('tracks, characters and competitions outside the game tables never reach the page', async () => {
  const env = makeHub();
  race(env, 10, [0, 1], { competition: 'survival', track: 19 });
  const history = env.hub.metricsHistory(30, 1);
  history.tracks.push({ value: '99', races: 50, completed: 0, abandoned: 0, duration_ms: 0 }, { value: 'PistaOutra', races: 50 });
  history.competitions.push({ value: 'debug', races: 50 });
  history.characters.push({ character: 40, picks: 50, races: 50, completed: 0 });
  const page = await open(env, { history });
  assert.equal(bars(page, 'd-track').length, 20);
  assert.equal(bars(page, 'd-track')[0].percent, '100%');
  assert.equal(bars(page, 'd-competition')[0].percent, '100%');
  assert.equal(bars(page, 'd-character').length, 12);
  assert.ok(!/99|PistaOutra|debug|Unknown/.test(page.text('d-track') + page.text('d-competition') + page.text('d-character')));
});

test('exactly the 9 competitions appear, by name', async () => {
  const env = makeHub();
  race(env, 10, [0], { competition: 'survival' });
  race(env, 11, [0], { competition: 'survival' });
  race(env, 12, [0], { competition: 'ctf' });
  const page = await open(env);
  const rows = bars(page, 'd-competition');
  assert.equal(rows.length, 9);
  assert.deepEqual(rows.map((r) => r.name).sort(), COMPETITIONS.map((c) => c.name).sort());
  assert.deepEqual(rows[0], { name: 'Survival', count: 2, percent: '67%' });
  assert.deepEqual(rows[1], { name: 'Capture the Flag', count: 1, percent: '33%' });
});

test('invalid ids are refused by the service and leave the dashboard untouched', async () => {
  const env = makeHub();
  const client = greeted(env, 10);
  const bad = [{ characters: [12] }, { characters: [-1] }, { characters: ['0'] }, { characters: [0, 1, 2, 3, 4] }, { track: 20 },
    { track: -1 }, { track: 'Pista' }, { competition: 'debug' }, { competition: 3 }];
  bad.forEach((extra, i) => {
    const reply = client.call('race.start', { race: i, competition: 'normal_race', ...extra });
    assert.equal(reply.code, 'bad_request', JSON.stringify(extra));
  });
  const page = await open(env);
  assert.equal(page.text('c-races-range'), '0');
  assert.deepEqual(bars(page, 'd-player-count'), []);
  assert.deepEqual(env.host.sql.all(`SELECT * FROM metrics_daily_characters`), []);
  assert.deepEqual(env.host.sql.all(`SELECT * FROM metrics_daily_race_outcomes`), []);
});

test('race starts and completions are counted separately, with a completion rate', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  race(env, 11, [0, 1]);
  race(env, 12, [0], { finish: false });
  const abandoned = race(env, 13, [0, 1], { finish: false });
  abandoned.drop();
  const page = await open(env);
  assert.deepEqual(['c-races-today', 'c-races-range'].map(page.text), ['4', '4']);
  const cards = Object.fromEntries(page.nodes.get('activity').find((n) => n.className === 'card')
    .map((n) => [n.children[0].textContent, n.children[1].textContent]));
  assert.equal(cards['Races started'], '4');
  assert.equal(cards['Races completed'], '2');
  assert.equal(cards['Completion rate'], '67%');
});

test('daily charts show the real value of each day', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  race(env, 11, [0, 1], { finish: false });
  env.clock.now += DAY;
  for (const n of [20, 21, 22]) race(env, n, [2, 3]);
  const page = await openRange(env, 7);
  assert.deepEqual(barLabels(page, 'h-races-started'), ['2', '3']);
  assert.deepEqual(barLabels(page, 'h-races-completed'), ['1', '3']);
  assert.deepEqual(barLabels(page, 'h-unique'), ['2', '3']);
  assert.deepEqual(barLabels(page, 'h-sessions'), ['2', '3']);
  assert.ok(page.nodes.get('h-peak-online').children.length > 0);
  const [head, latest, earlier] = page.rows('daily');
  assert.deepEqual(head, ['Day', 'Unique players', 'Sessions', 'Peak online', 'Rooms created', 'Joins', 'Races started', 'Races completed']);
  assert.deepEqual([latest[1], latest[6], latest[7]], ['3', '3', '3']);
  assert.deepEqual([earlier[1], earlier[6], earlier[7]], ['2', '2', '1']);
});

test('the comparison sets the range against the same number of days before it', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  race(env, 11, [0, 1, 2]);
  env.clock.now += 8 * DAY;
  race(env, 12, [0, 1]);
  race(env, 13, [0, 1]);
  race(env, 14, [0, 1]);
  const page = await openRange(env, 7);
  const rows = compareRows(page);
  assert.equal(page.rows('compare')[0].join('|'), 'Metric|This period|Previous period|Change');
  assert.deepEqual(rows['Races started'], ['3', '2', '+1 (+50%)']);
  assert.deepEqual(rows['2-player races'], ['3', '1', '+2 (+200%)']);
  assert.deepEqual(rows['3-player races'], ['0', '1', '-1 (-100%)']);
  assert.deepEqual(rows['Unique players'], ['3', '2', '+1 (+50%)']);
  const history = env.hub.metricsHistory(7, 1);
  assert.equal(history.previous.races_started, 2);
  assert.match(page.text('compare-note'), new RegExp(`${history.first} to ${history.last} compared with `));
});

test('a previous period of zero shows new or a dash, never a percentage', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  const page = await openRange(env, 7);
  const rows = compareRows(page);
  assert.equal(rows['Races started'][2], 'new');
  assert.equal(rows['4-player races'][2], '–');
  assert.ok(!page.rows('compare').some((r) => r[3].includes('Infinity') || r[3].includes('NaN')));
});

test('a range with no data says so and shows no invented value', async () => {
  const page = await open(makeHub());
  for (const id of ['c-online', 'c-in-rooms', 'c-rooms', 'c-peak', 'c-unique-today', 'c-unique-range', 'c-races-today', 'c-races-range']) {
    assert.equal(page.text(id), '0', id);
  }
  assert.match(page.text('d-party-size'), /No rooms have ended in this range yet/);
  assert.match(page.text('d-player-count'), /No races have been reported in this range yet/);
  assert.match(page.text('d-character'), /No races with known characters/);
  assert.match(page.text('d-combination'), /No multiplayer races with known characters/);
  assert.match(page.text('d-track'), /No races with a known track/);
  assert.match(page.text('d-competition'), /No races have been reported/);
  assert.equal(bars(page, 'd-track').length, 0);
  const completion = page.nodes.get('activity').find((n) => n.className === 'card').find((n) => n.children[0].textContent === 'Completion rate');
  assert.equal(completion.children[1].textContent, '–');
  assert.ok(!/NaN|Infinity|undefined/.test(page.all()));
});

test('cards and rows explain themselves on hover', async () => {
  const env = makeHub();
  race(env, 10, [0, 1]);
  const page = await open(env);
  const card = page.nodes.get('k-unique-today');
  assert.equal(card.attrs['data-help'].includes('anonymous key'), true);
  card.listeners.mousemove[0]({ clientX: 1, clientY: 1 });
  assert.match(page.text('tip'), /counted once today/);
  const row = page.nodes.get('d-combination').find((n) => n.className === 'row')[0];
  row.listeners.mousemove[0]({ clientX: 1, clientY: 1 });
  assert.match(page.text('tip'), /Chaves \+ Quico/);
  assert.match(page.text('tip'), /2 players/);
  assert.match(page.text('tip'), /1 race · 100%/);
});

test('the technical clutter is gone from the main page and one collapsed section remains', () => {
  const main = ADMIN_HTML.slice(0, ADMIN_HTML.indexOf('<details'));
  const removed = /Connection health|WebSocket|Sign-ins|STUN|UPnP|Protection counters|Abuse protection|Diagnostics|Current rooms|Last rooms|How to read this|Rooms advertised|Races in progress|Direct now|Relay now|Pending now|Occupied seats|Maximum seats|Average occupancy|Room lifetime|Race duration/i;
  assert.ok(!removed.test(main), main.match(removed)?.[0]);
  assert.ok(!removed.test(ADMIN_JS), ADMIN_JS.match(removed)?.[0]);
  assert.ok(!/Country|Platform|Port version/.test(main));
  assert.equal(ADMIN_HTML.split('<details').length, 2);
  assert.ok(!ADMIN_HTML.slice(ADMIN_HTML.indexOf('<details')).includes('<details open'));
  assert.ok(ADMIN_HTML.includes('<summary>Technical details</summary>'));
});

test('the technical section is compact and filled from the same history', async () => {
  const env = makeHub();
  party(env, 2, 2, { first: 10 });
  const page = await open(env);
  const cards = page.nodes.get('technical-cards').find((n) => n.className === 'card').map((n) => n.children[0].textContent);
  assert.deepEqual(cards, ['Relay rooms', 'Direct rooms', 'Rooms with no path', 'Protocol rejects', 'Refused by limits']);
  assert.match(page.text('d-platform'), /Windows/);
});

test('the page shows no player name, friend code, address, token or room id', async () => {
  const env = makeHub({ local: false });
  const ip = '198.51.100.23';
  const name = 'Quiquinho99';
  const p = party(env, 2, 2, { first: 10, options: { ip, name } });
  const hostToken = p.host.who.token;
  race(env, 40, [0, 1]).call('race.start', { race: 1, competition: 'coins', characters: [2, 3] });
  const page = await open(env);
  const history = JSON.stringify(env.hub.metricsHistory(30, 50));
  const summary = JSON.stringify(env.hub.metricsSummary());
  for (const text of [page.all(), ADMIN_HTML, ADMIN_JS, history, summary]) {
    for (const secret of [name, ip, hostToken, p.room, p.host.who.player, p.guests[0].who.player, String(p.token)]) {
      assert.ok(!text.includes(secret), secret);
    }
  }
  const stored = env.host.sql.all('SELECT * FROM metrics_players');
  assert.ok(stored.length >= 3);
  for (const row of stored) {
    assert.match(row.pseudonym, /^[0-9a-f]{32}$/);
    for (const code of [p.host.who.player, p.guests[0].who.player]) assert.ok(!row.pseudonym.includes(code));
  }
});

test('the page keeps the token out of URLs and loads nothing from elsewhere', async () => {
  const env = makeHub();
  const page = await openRange(env, 7);
  assert.ok(page.requests.includes('/admin/api/summary'));
  assert.ok(!page.requests.join('').includes('token'));
  assert.ok(!/https?:\/\//.test(ADMIN_HTML + ADMIN_JS.replace('http://www.w3.org/2000/svg', '')));
  assert.ok(!/<script src="https?:|<link [^>]*href="https?:|cdn\./i.test(ADMIN_HTML));
});

test('operational telemetry stays in the admin API even though the page no longer shows it', () => {
  const env = makeHub({ local: false });
  const p = party(env, 2, 2, { first: 10 });
  p.host.call('match.transport', { room: p.room, seat: 1, transport: 'relay' });
  const history = env.hub.metricsHistory(30, 20);
  for (const key of ['abuse', 'reject', 'transport', 'room_capacity', 'room_occupancy', 'room_lifetime', 'country', 'platform', 'version']) {
    assert.ok(Array.isArray(history.dimensions[key]), key);
  }
  assert.ok(Array.isArray(history.recent.rooms));
  assert.ok(Array.isArray(env.hub.metricsSummary().live.list));
  assert.equal(env.hub.metricsSummary().live.relay, 1);
  assert.ok(history.dimensions.transport.some((t) => t.value === 'relay'));
});
