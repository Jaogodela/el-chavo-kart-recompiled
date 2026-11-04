// Runs the real Worker and Durable Object class against a stand-in for the Cloudflare runtime, so the routing, the
// headers the edge sets and the object's wiring to the hub are tested as they are deployed.
import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';
import { DatabaseSync } from 'node:sqlite';
import { test } from 'node:test';

import { GAME_ID, PROTOCOL_VERSION, SERVICE, WORKER_BUILD } from '../src/protocol.js';
import { IMAGE, identity } from './harness.js';

registerHooks({
  resolve(specifier, context, next) {
    if (specifier !== 'cloudflare:workers') return next(specifier, context);
    return { url: 'data:text/javascript,export class DurableObject { constructor(ctx, env) { this.ctx = ctx; this.env = env; } }',
      shortCircuit: true };
  },
});

class FakeSocket {
  constructor() {
    this.sent = [];
    this.stored = null;
    this.closed = null;
  }

  deserializeAttachment() {
    return structuredClone(this.stored);
  }

  serializeAttachment(value) {
    this.stored = structuredClone(value);
  }

  send(data) {
    this.sent.push(data);
  }

  close(code, reason) {
    this.closed = { code, reason };
  }
}

const NativeResponse = globalThis.Response;

// workerd accepts status 101 with a webSocket; Node's Response does not.
class WorkerResponse extends NativeResponse {
  constructor(body, init = {}) {
    super(body, init.status === 101 ? { ...init, status: 200 } : init);
    if (init.status === 101) Object.defineProperties(this, { status: { value: 101 }, webSocket: { value: init.webSocket } });
  }
}

globalThis.Response = WorkerResponse;
globalThis.WebSocketPair = function WebSocketPair() {
  return { 0: new FakeSocket(), 1: new FakeSocket() };
};
globalThis.WebSocketRequestResponsePair = class {
  constructor(request, response) {
    Object.assign(this, { request, response });
  }
};

const { default: worker, ECKLobby } = await import('../src/worker.js');

function fakeContext() {
  const db = new DatabaseSync(':memory:');
  const sockets = [];
  const state = { alarm: null, autoResponse: null };
  return {
    state,
    sockets,
    setWebSocketAutoResponse: (pair) => { state.autoResponse = pair; },
    acceptWebSocket: (ws) => sockets.push(ws),
    getWebSockets: () => sockets,
    blockConcurrencyWhile: (fn) => fn(),
    storage: {
      getAlarm: async () => state.alarm,
      setAlarm: (ms) => { state.alarm = ms; },
      sql: { exec: (query, ...args) => {
        const statement = db.prepare(query);
        const rows = statement.columns().length ? statement.all(...args).map((r) => ({ ...r })) : (statement.run(...args), []);
        return { toArray: () => rows };
      } },
    },
  };
}

async function deployment(extra = {}) {
  const ctx = fakeContext();
  const object = new ECKLobby(ctx, {});
  await Promise.resolve();
  const env = { LOBBY: { idFromName: (name) => name, get: () => ({ fetch: (input) => object.fetch(input instanceof Request ? input : new Request(input)) }) },
    ...extra };
  const request = (path, init = {}) => worker.fetch(new Request(`https://eck-lobby.example${path}`, init), env);
  const upgrade = async (ip = '203.0.113.5', headers = {}, cf = undefined) => {
    const forwarded = new Request('https://eck-lobby.example/', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': ip, ...headers } });
    if (cf) Object.defineProperty(forwarded, 'cf', { value: cf });
    const response = await worker.fetch(forwarded, env);
    return { response, server: ctx.sockets.at(-1) };
  };
  const hello = (server, n) => {
    const who = identity(n);
    object.webSocketMessage(server, JSON.stringify({ op: 'hello', seq: 1, proto: PROTOCOL_VERSION, game: GAME_ID, image: IMAGE, build: 1,
      player: who.player, token: who.token, name: 'Net', version: '0.1.0', platform: 'Windows' }));
    return JSON.parse(server.sent.at(-1));
  };
  return { ctx, object, env, request, upgrade, hello };
}

test('GET / and /health report the service, and other paths are 404', async () => {
  const { request } = await deployment();
  for (const path of ['/', '/health']) {
    const response = await request(path);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('Cache-Control'), 'no-store');
    assert.deepEqual(await response.json(), { service: SERVICE, game: GAME_ID, protocol: PROTOCOL_VERSION, status: 'ok',
      worker_build: WORKER_BUILD, relay_supported: true, relay_max_payload: 1472 });
  }
  assert.equal(SERVICE, 'eck-lobby');
  assert.equal(GAME_ID, 'el-chavo-kart-pc');
  assert.equal((await request('/nope')).status, 404);
  assert.equal((await request('/health', { method: 'POST' })).status, 404);
});

test('a WebSocket upgrade reaches the object, is accepted and answers a hello', async () => {
  const { ctx, upgrade, hello } = await deployment();
  const { response, server } = await upgrade();
  assert.equal(response.status, 101);
  assert.ok(response.webSocket);
  assert.equal(hello(server, 1).code, 'ok');
  assert.equal(ctx.state.autoResponse.request, 'ping');
  assert.equal(ctx.state.autoResponse.response, 'pong');
  assert.notEqual(ctx.state.alarm, null);
});

test('a refused upgrade is a 429 and creates no socket', async () => {
  const { ctx, upgrade } = await deployment({ WS_UPGRADE_LIMITER: { limit: async () => ({ success: false }) } });
  const { response } = await upgrade();
  assert.equal(response.status, 429);
  assert.equal(response.headers.get('Retry-After'), '60');
  assert.equal(ctx.sockets.length, 0);
});

test('the socket cap of an address is a 429 from the object', async () => {
  const { upgrade } = await deployment();
  for (let i = 0; i < 16; i++) assert.equal((await upgrade('203.0.113.9')).response.status, 101);
  assert.equal((await upgrade('203.0.113.9')).response.status, 429);
  assert.equal((await upgrade('203.0.113.10')).response.status, 101);
});

test('the country comes from the edge: a forged header does not reach the metrics', async () => {
  const { object, upgrade, hello } = await deployment();
  const { server } = await upgrade('203.0.113.5', { 'X-ECK-Edge-Country': 'ZZ' });
  hello(server, 1);
  assert.deepEqual(object.hub.metricsSummary().live.country, [{ value: 'Unknown', count: 1 }]);
  const edge = await upgrade('203.0.113.6', { 'X-ECK-Edge-Country': 'ZZ' }, { country: 'BR' });
  hello(edge.server, 2);
  assert.deepEqual(object.hub.metricsSummary().live.country, [{ value: 'BR', count: 1 }, { value: 'Unknown', count: 1 }]);
});

test('a client cannot reach the metrics routes of the object from outside', async () => {
  const { request } = await deployment();
  assert.equal((await request('/metrics/summary')).status, 404);
  const upgraded = await request('/metrics/summary', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': '203.0.113.5' } });
  assert.equal(upgraded.status, 101);
});

test('the dashboard API reads the object through the Worker, with the ECK token only', async () => {
  const TOKEN = 'dashboard-token-0123456789';
  const { request, upgrade, hello } = await deployment({ ECK_DASHBOARD_TOKEN: TOKEN });
  const { server } = await upgrade();
  hello(server, 1);
  const headers = { Authorization: `Bearer ${TOKEN}` };
  const summary = await request('/admin/api/summary', { headers });
  assert.equal(summary.status, 200);
  assert.equal((await summary.json()).live.online, 1);
  const history = await request('/admin/api/history?days=7&recent=10', { headers });
  assert.equal((await history.json()).days, 7);
  assert.equal((await request('/admin/api/summary')).status, 401);
  assert.equal((await request('/admin')).status, 200);
});

test('oversized and binary frames take the routes they should; a closed socket ends its presence', async () => {
  const { ctx, object, upgrade, hello } = await deployment();
  const { server } = await upgrade();
  hello(server, 1);
  object.webSocketMessage(server, 'x'.repeat(5000));
  assert.equal(server.closed.code, 1009);
  object.webSocketMessage(server, new ArrayBuffer(4));
  object.webSocketClose(server, 1005);
  assert.equal(server.closed.code, 1000);
  ctx.sockets.length = 0;
  assert.equal(object.hub.metricsSummary().live.online, 0);
});

test('hibernation: a rebuilt object knows its alarm and keeps its sockets', async () => {
  const { ctx, object, env, upgrade, hello } = await deployment();
  const { server } = await upgrade();
  hello(server, 1);
  const sets = [];
  const setAlarm = ctx.storage.setAlarm;
  ctx.storage.setAlarm = (ms) => {
    sets.push(ms);
    setAlarm(ms);
  };
  const woken = new ECKLobby(ctx, env);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(sets.length, 0);
  assert.equal(woken.hub.metricsSummary().live.online, 1);
  woken.webSocketMessage(server, JSON.stringify({ op: 'ping', seq: 2 }));
  assert.equal(JSON.parse(server.sent.at(-1)).code, 'ok');
  assert.ok(object);
});
