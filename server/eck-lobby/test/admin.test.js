import assert from 'node:assert/strict';
import { test } from 'node:test';

import { COUNTRY_HEADER, handleAdmin, internalMetricsRoute, tokenMatches } from '../src/admin.js';
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { metricsDay } from '../src/metrics.js';
import { greeted, identity, joinRoom, makeHub, befriend, party } from './harness.js';

const TOKEN = 'dashboard-token-0123456789';
const ENV = { ECK_DASHBOARD_TOKEN: TOKEN };

const get = (path, headers = {}) => new Request(`https://eck-lobby.example${path}`, { headers });
const bearer = (token) => ({ Authorization: `Bearer ${token}` });

function source(env) {
  return {
    summary: async () => JSON.parse(JSON.stringify(env.hub.metricsSummary())),
    history: async (days, recent) => JSON.parse(JSON.stringify(env.hub.metricsHistory(days, recent))),
  };
}

async function summaryOf(env) {
  const response = await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), ENV, source(env));
  assert.equal(response.status, 200);
  return response.json();
}

const PAGE = ['/admin', '/admin/', '/admin/app.css', '/admin/app.js'];
const API = ['/admin/api/summary', '/admin/api/history?days=30'];

test('GET /admin serves the dashboard page without a token and without any data', async () => {
  for (const path of ['/admin', '/admin/']) {
    const response = await handleAdmin(get(path), ENV, source(makeHub()));
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('Content-Type'), 'text/html; charset=utf-8');
    const html = await response.text();
    assert.match(html, /<title>ECK Online Metrics<\/title>/);
    assert.match(html, /El Chavo Kart · Online Metrics/);
    for (const label of ['Online now', 'Players in rooms', 'Open rooms', 'Peak today', 'Unique players today', 'Races today',
      'Period comparison', 'Multiplayer activity', 'Players per race', 'Characters', 'Character combinations', 'Tracks', 'Competitions',
      'Historical activity', 'Daily totals', 'Technical details']) {
      assert.ok(html.includes(label), label);
    }
    assert.ok(!html.includes(TOKEN));
  }
  for (const [path, type] of [['/admin/app.js', 'text/javascript'], ['/admin/app.css', 'text/css']]) {
    const response = await handleAdmin(get(path), ENV, source(makeHub()));
    assert.equal(response.status, 200);
    assert.ok(response.headers.get('Content-Type').startsWith(type));
  }
});

test('every dashboard answer carries the security headers and a strict CSP', async () => {
  const env = makeHub();
  const answers = [...PAGE.map((p) => get(p)), ...API.map((p) => get(p, bearer(TOKEN))), get('/admin/api/summary'), get('/admin/nope'),
    new Request('https://x.example/admin', { method: 'POST' })];
  for (const request of answers) {
    const { headers } = await handleAdmin(request, ENV, source(env));
    assert.equal(headers.get('Cache-Control'), 'no-store');
    assert.equal(headers.get('X-Content-Type-Options'), 'nosniff');
    assert.equal(headers.get('Referrer-Policy'), 'no-referrer');
    assert.equal(headers.get('X-Frame-Options'), 'DENY');
    assert.equal(headers.get('Cross-Origin-Opener-Policy'), 'same-origin');
    assert.equal(headers.get('Cross-Origin-Resource-Policy'), 'same-origin');
    assert.equal(headers.get('Content-Security-Policy'), "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
      "img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  }
});

test('the page writes values as text, never markup, and loads nothing from elsewhere', () => {
  assert.ok(ADMIN_CSS.includes('.dist.big'));
  assert.ok(!/<script>|<style>| style=| on[a-z]+=/i.test(ADMIN_HTML));
  assert.ok(!/\beval\(|new Function|innerHTML|document\.cookie|[?&]token=/.test(ADMIN_JS));
  for (const text of [ADMIN_HTML, ADMIN_CSS, ADMIN_JS]) assert.ok(!/(src|href)=["']?https?:|@import|url\(/i.test(text));
  assert.doesNotThrow(() => new Function(ADMIN_JS));
  assert.match(ADMIN_JS, /Authorization: 'Bearer ' \+ token/);
});

test('the API needs the ECK token: none, a wrong one or a near miss is refused', async () => {
  const env = makeHub();
  for (const headers of [{}, bearer('wrong-token-wrong-token-wrong'), bearer(`${TOKEN}x`), bearer(TOKEN.slice(0, -1)), { Authorization: TOKEN },
    { Authorization: `Basic ${TOKEN}` }]) {
    for (const path of API) {
      const response = await handleAdmin(get(path, headers), ENV, source(env));
      assert.equal(response.status, 401);
      assert.equal(response.headers.get('WWW-Authenticate'), 'Bearer');
    }
  }
  assert.equal(tokenMatches(`Bearer ${TOKEN}`, TOKEN), true);
});

test('without a usable token on the service the API says so, and no other game\'s secret opens it', async () => {
  const env = makeHub();
  assert.equal((await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), {}, source(env))).status, 503);
  const short = await handleAdmin(get('/admin/api/summary', bearer('short')), { ECK_DASHBOARD_TOKEN: 'short' }, source(env));
  assert.equal(short.status, 503);
  const fifteen = 'x'.repeat(15);
  assert.equal(tokenMatches(`Bearer ${fifteen}`, fifteen), false);
  assert.equal((await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), { DASHBOARD_TOKEN: TOKEN }, source(env))).status, 503);
});

test('the dashboard is read-only and validates its parameters', async () => {
  const env = makeHub();
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE']) {
    for (const path of ['/admin', '/admin/api/summary']) {
      const response = await handleAdmin(new Request(`https://x.example${path}`, { method, headers: bearer(TOKEN) }), ENV, source(env));
      assert.equal(response.status, 405);
      assert.equal(response.headers.get('Allow'), 'GET');
    }
  }
  for (const query of ['days=366', 'days=0', 'days=abc', 'days=1.5', 'days=-1', 'recent=51', 'recent=0', 'recent=x']) {
    assert.equal((await handleAdmin(get(`/admin/api/history?${query}`, bearer(TOKEN)), ENV, source(env))).status, 400, query);
  }
  assert.equal((await handleAdmin(get('/admin/api/history?days=365&recent=50', bearer(TOKEN)), ENV, source(env))).status, 200);
  assert.equal((await handleAdmin(get('/admin/nope', bearer(TOKEN)), ENV, source(env))).status, 404);
});

test('an unreachable Durable Object answers 502 without a stack or secret', async () => {
  const logs = [];
  const failing = { summary: async () => { throw new Error('lobby answered 500'); }, history: async () => ({}) };
  const response = await handleAdmin(get('/admin/api/summary', bearer(TOKEN)), ENV, failing, (text) => logs.push(text));
  assert.equal(response.status, 502);
  const text = await response.text();
  assert.equal(text, '{"error":"unavailable"}');
  assert.ok(!logs.join('\n').includes(TOKEN));
});

test('an idle service reports zeros and an empty room list', async () => {
  const env = makeHub();
  const summary = await summaryOf(env);
  assert.equal(summary.day, metricsDay(env.clock.now));
  assert.deepEqual([summary.live.online, summary.live.rooms, summary.live.max_seats, summary.live.average_occupancy], [0, 0, 0, 0]);
  assert.deepEqual(summary.live.list, []);
  const history = await (await handleAdmin(get('/admin/api/history?days=7', bearer(TOKEN)), ENV, source(env))).json();
  assert.equal(history.daily.length, 7);
  assert.deepEqual(history.samples, []);
});

test('the country is the edge header; a client never chooses it', async () => {
  const env = makeHub();
  env.connect(identity(1), { country: 'BR' }).hello({ country: 'US' });
  env.connect(identity(2), { country: 'not a country' }).hello();
  const summary = await summaryOf(env);
  assert.deepEqual(summary.live.country, [{ value: 'BR', count: 1 }, { value: 'Unknown', count: 1 }]);
  assert.equal(COUNTRY_HEADER, 'X-ECK-Edge-Country');
});

test('the dashboard exposes no relay token, connection id, address, name or friend code', async () => {
  const env = makeHub();
  const { host, guests, room, token } = party(env, 2, 2, { options: { ip: '198.51.100.23' } });
  host.call('match.transport', { room, seat: 1, transport: 'relay' });
  host.call('match.endpoint', { room, port: 40000, public_ip: '198.51.100.23', public_port: 40001 });
  const seats = env.host.sql.all('SELECT cid FROM room_seats');
  const summaryText = JSON.stringify(await summaryOf(env));
  const historyText = await (await handleAdmin(get('/admin/api/history?days=30', bearer(TOKEN)), ENV, source(env))).text();
  const pageText = ADMIN_HTML + ADMIN_JS + ADMIN_CSS;
  assert.ok(token > 0);
  for (const text of [summaryText, historyText, pageText]) {
    assert.ok(!text.includes(room), 'full room id');
    for (const seat of seats) assert.ok(!text.includes(seat.cid), 'connection id');
    assert.ok(!text.includes('198.51.100.23'), 'address');
    assert.ok(!text.includes(host.who.player) && !text.includes(guests[0].who.player), 'friend code');
    assert.ok(!text.includes(host.who.token), 'player token');
    assert.ok(!text.includes(host.fields.name), 'name');
    assert.ok(!/relay_token|token_hash|salt/.test(text), 'field names');
    assert.ok(!text.includes(TOKEN));
  }
  assert.deepEqual(Object.keys((await summaryOf(env)).live.list[0]).sort(),
    ['age_ms', 'capacity', 'connected', 'direct', 'endpoints', 'id', 'occupied', 'pending', 'relay', 'relay_available', 'state']);
});

test('the Durable Object serves metrics only outside a WebSocket upgrade, and parameters are checked again there', () => {
  const route = (path, headers = {}) => internalMetricsRoute(get(path, headers));
  assert.deepEqual(route('/metrics/summary'), { kind: 'summary' });
  assert.deepEqual(route('/metrics/history?days=90'), { kind: 'history', days: 90, recent: 20 });
  assert.deepEqual(route('/metrics/history?days=7&recent=50'), { kind: 'history', days: 7, recent: 50 });
  assert.deepEqual(route('/metrics/history?days=7&recent=51'), { kind: 'bad_request' });
  assert.deepEqual(route('/metrics/history?days=9999'), { kind: 'bad_request' });
  assert.equal(route('/metrics/summary', { Upgrade: 'websocket' }), null);
  assert.equal(route('/'), null);
});

test('a real session shows on the dashboard', async () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  a.call('room.host', { capacity: 4 });
  joinRoom(a, b);
  const summary = await summaryOf(env);
  assert.deepEqual([summary.live.online, summary.live.in_rooms, summary.live.rooms_4, summary.live.pending], [2, 2, 1, 1]);
  assert.equal(summary.today.peak_in_rooms, 2);
});
