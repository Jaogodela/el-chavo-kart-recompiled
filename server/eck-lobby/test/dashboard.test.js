import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ADMIN_HTML } from '../src/admin_page.js';
import { openDashboard } from './dom.js';
import { makeHub, party } from './harness.js';

const MINUTE = 60 * 1000;

function scenario() {
  const env = makeHub();
  const one = party(env, 3, 4, { first: 10, options: { platform: 'Windows' } });
  one.host.call('match.transport', { room: one.room, seat: 1, transport: 'direct' });
  one.host.call('match.transport', { room: one.room, seat: 2, transport: 'relay' });
  one.host.call('room.start', { room: one.room });
  for (const c of one.clients) c.call('match.ready', { room: one.room, ready: true });
  env.advance(7 * MINUTE);
  one.host.call('room.leave', { room: one.room });
  const two = party(env, 2, 3, { first: 20 });
  two.host.call('match.transport', { room: two.room, seat: 1, transport: 'relay' });
  const three = party(env, 2, 2, { first: 30 });
  return { env, one, two, three };
}

const open = async (env, options = {}) => openDashboard({ summary: env.hub.metricsSummary(),
  history: env.hub.metricsHistory(30, 20), ...options });

test('live cards say now and the accumulated cards say today', () => {
  for (const label of ['Online now', 'In rooms now', 'Rooms advertised now', 'Active rooms now', 'Direct now', 'Relay now', 'Pending now',
    'Peak online today', 'Sessions today', 'Rooms created today', 'Rooms started today', 'Rooms closed today', 'Joins today', 'Invites today',
    'Direct selected today', 'Relay selected today']) {
    assert.ok(ADMIN_HTML.includes(`>${label}<`), label);
  }
});

test('the sections follow the documented page order', () => {
  const order = ['Live now', 'Today', 'Selected range', 'Period comparison', 'Current rooms', 'Last rooms', 'Usage over time', 'Sessions by day',
    'Rooms by day', 'Room occupancy', '<h2>Platform</h2>', '<h2>Transport</h2>', 'Daily totals', 'Diagnostics'];
  let at = -1;
  for (const marker of order) {
    const next = ADMIN_HTML.indexOf(marker);
    assert.ok(next > at, marker);
    at = next;
  }
});

test('the live cards show the current state, by seat and by room size', async () => {
  const { env } = scenario();
  const page = await open(env);
  assert.equal(page.text('c-online'), '7');
  assert.equal(page.text('c-in-rooms'), '4');
  assert.equal(page.text('c-advertised'), '2');
  assert.equal(page.text('c-active'), '0');
  assert.equal(page.text('c-relay'), '1');
  assert.equal(page.text('c-pending'), '1');
  assert.equal(page.text('c-direct'), '0');
  assert.deepEqual([page.text('c-rooms-2'), page.text('c-rooms-3'), page.text('c-rooms-4')], ['1', '1', '0']);
  assert.deepEqual([page.text('c-occupied'), page.text('c-max-seats'), page.text('c-occupancy')], ['4', '5', '80%']);
});

test('average occupancy is a dash with no rooms', async () => {
  const page = await open(makeHub());
  assert.equal(page.text('c-occupancy'), '–');
  assert.equal(page.text('c-online'), '0');
});

test('today and the range show the accumulated numbers', async () => {
  const { env } = scenario();
  const page = await open(env);
  assert.deepEqual(['t-created', 't-started', 't-closed', 't-joins', 't-direct', 't-relay'].map(page.text), ['3', '1', '1', '4', '1', '2']);
  assert.equal(page.text('t-sessions'), '7');
  for (const days of [7, 30, 90]) {
    const ranged = await open(env, { range: String(days), historyFor: () => env.hub.metricsHistory(days, 20) });
    assert.match(ranged.text('range-title'), new RegExp(`last ${days} days`));
    assert.deepEqual(['r-sessions', 'r-created', 'r-started', 'r-closed', 'r-guest', 'r-joins', 'r-rooms-relay', 'r-rooms-direct'].map(ranged.text),
      ['7', '3', '1', '1', '1', '4', '1', '0']);
  }
});

test('the comparison table sets this period against the one before, with a dash when nothing changed', async () => {
  const env = makeHub();
  const a = party(env, 2, 2, { first: 10 });
  a.host.call('room.leave', { room: a.room });
  env.clock.now += 8 * 24 * 3600 * 1000;
  for (const first of [20, 30]) party(env, 2, 2, { first });
  const page = await open(env, { range: '7', historyFor: () => env.hub.metricsHistory(7, 20) });
  const rows = Object.fromEntries(page.rows('compare').map((r) => [r[0], r.slice(1)]));
  assert.deepEqual(rows.Metric, ['This period', 'Previous period', 'Change']);
  assert.deepEqual(rows['Rooms created'], ['2', '1', '+1 · +100%']);
  assert.deepEqual(rows.Invites, ['0', '0', '–']);
  assert.match(page.text('compare-note'), /compared with the 7 days before/);
});

test('current rooms stay a live table with a short id only', async () => {
  const { env, two } = scenario();
  const page = await open(env);
  const rows = page.rows('rooms');
  assert.equal(rows.length, 3);
  const row = rows.find((r) => r[0] === two.room.slice(0, 6));
  assert.deepEqual(row.slice(1, 8), ['Open', '2 / 3', '2 / 2', '0', '1', '0', 'Available']);
  assert.ok(!page.all().includes(two.room));
});

test('last rooms shows a header and nothing when empty', async () => {
  const page = await open(makeHub());
  assert.equal(page.text('recent-title'), 'Last rooms · showing 0 rooms');
  assert.equal(page.text('recent-empty'), 'No finished rooms in this range.');
  assert.equal(page.rows('recent').length, 0);
});

test('last rooms lists a closed room with no identifying field', async () => {
  const { env } = scenario();
  const page = await open(env);
  assert.equal(page.text('recent-title'), 'Last rooms · showing 1 room');
  const [head, row] = page.rows('recent');
  assert.deepEqual(head, ['Date and time', 'Duration', 'Peak occupancy', 'Transport', 'Platforms', 'Final state']);
  assert.deepEqual(row.slice(1), ['7m 0s', '3 / 4', 'Mixed', 'Windows', 'Ended']);
});

test('room size, occupancy and lifetime read in their natural order', async () => {
  const env = makeHub();
  let first = 10;
  for (const [size, capacity] of [[4, 4], [2, 4], [2, 2], [3, 3], [2, 3]]) {
    const p = party(env, size, capacity, { first });
    first += 10;
    p.host.call('room.leave', { room: p.room });
  }
  const page = await open(env);
  const text = page.text('d-occupancy');
  assert.ok(text.indexOf('2/2') < text.indexOf('2/3') && text.indexOf('2/3') < text.indexOf('3/3') && text.indexOf('3/3') < text.indexOf('2/4') &&
    text.indexOf('2/4') < text.indexOf('4/4'), text);
  assert.match(page.text('d-capacity'), /4 seats/);
  assert.match(page.text('d-lifetime'), /Under 1 minute/);
});

test('charts, breakdowns and diagnostics render', async () => {
  const { env } = scenario();
  const page = await open(env);
  for (const id of ['usage', 'sessions', 'rooms-by-day', 'd-platform', 'd-version', 'd-country', 'd-transport', 'daily', 'rejects']) {
    assert.ok(page.nodes.get(id).children.length > 0, id);
  }
  assert.match(page.text('status'), /Updated/);
  assert.match(page.text('d-transport'), /Direct/);
});

test('the requests carry the selected range and list size, never the token in a URL', async () => {
  const env = makeHub();
  const page = await open(env, { range: '7', recent: '50', historyFor: () => env.hub.metricsHistory(7, 50) });
  assert.ok(page.requests.includes('/admin/api/summary'));
  assert.ok(page.requests.includes('/admin/api/history?days=7&recent=50'));
  assert.ok(!page.requests.join('').includes('token'));
});
