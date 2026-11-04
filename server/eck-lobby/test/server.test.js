import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import { startLobbyServer } from '../local/server.js';
import { GAME_ID, PROTOCOL_VERSION, RELAY, SERVICE, WORKER_BUILD, encodeRelayFrame } from '../src/protocol.js';
import { IMAGE, identity } from './harness.js';

let server;
let base;

before(async () => {
  server = await startLobbyServer({ port: 0 });
  base = `127.0.0.1:${server.port}`;
});

after(async () => {
  await server.close();
});

class Socket {
  static async open() {
    const s = new Socket();
    s.ws = new WebSocket(`ws://${base}/`);
    s.ws.binaryType = 'arraybuffer';
    s.messages = [];
    s.seq = 0;
    s.ws.addEventListener('message', (e) => {
      if (e.data instanceof ArrayBuffer) s.messages.push({ binary: new Uint8Array(e.data) });
      else s.messages.push(e.data === 'pong' ? 'pong' : JSON.parse(e.data));
    });
    await new Promise((resolve, reject) => {
      s.ws.addEventListener('open', resolve, { once: true });
      s.ws.addEventListener('error', () => reject(new Error('handshake failed')), { once: true });
    });
    return s;
  }

  async next(match, timeout = 2000) {
    const deadline = Date.now() + timeout;
    for (;;) {
      const i = this.messages.findIndex(match);
      if (i >= 0) return this.messages.splice(i, 1)[0];
      if (Date.now() > deadline) throw new Error('timed out');
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  }

  async call(op, body = {}) {
    const seq = ++this.seq;
    this.ws.send(JSON.stringify({ op, seq, ...body }));
    return this.next((m) => m.re === seq);
  }

  hello(who, extra = {}) {
    return this.call('hello', { proto: PROTOCOL_VERSION, game: GAME_ID, image: IMAGE, build: 1, player: who.player,
      token: who.token, name: 'Net', version: '0.1.0', platform: 'Windows', relay: true, ...extra });
  }
}

test('health endpoint reports service, protocol, status and relay support only', async () => {
  const response = await fetch(`http://${base}/health`);
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { service: SERVICE, game: GAME_ID, protocol: PROTOCOL_VERSION, status: 'ok',
    worker_build: WORKER_BUILD, relay_supported: true, relay_max_payload: RELAY.maxPayload });
  assert.equal((await fetch(`http://${base}/nope`)).status, 404);
});

test('a probe-style session says whether the protocol is compatible', async () => {
  const probe = await Socket.open();
  assert.equal((await probe.hello(identity(1), { presence: false })).code, 'ok');
  const bad = await Socket.open();
  assert.equal((await bad.call('hello', { proto: 99 })).code, 'proto');
  assert.equal((await bad.call('ping')).code, 'no_session');
});

test('a 3 player room over real WebSockets: friends, search, join, endpoints, relay frames by seat', async () => {
  const sockets = await Promise.all([Socket.open(), Socket.open(), Socket.open()]);
  const ids = [identity(21), identity(22), identity(23)];
  for (const [i, s] of sockets.entries()) assert.equal((await s.hello(ids[i])).code, 'ok');
  const [a, b, c] = sockets;
  for (const [s, id] of [[b, ids[1]], [c, ids[2]]]) {
    assert.equal((await a.call('friends.add', { code: id.player })).state, 'pending');
    assert.equal((await s.next((m) => m.ev === 'friend.request')).player, ids[0].player);
    assert.equal((await s.call('friends.accept', { player: ids[0].player })).code, 'ok');
  }
  const hosted = await a.call('room.host', { capacity: 3, rules: { mode: 1 } });
  assert.equal(hosted.seat, 0);
  const seats = [];
  for (const s of [b, c]) {
    const found = await s.call('room.search');
    assert.equal(found.rooms[0].host, ids[0].player);
    seats.push((await s.call('room.join', { handle: found.rooms[0].handle })).seat);
  }
  assert.deepEqual(seats, [1, 2]);
  const full = await a.next((m) => m.ev === 'room.updated' && m.room.players === 3);
  assert.deepEqual(full.room.seats.map((s) => s.seat), [0, 1, 2]);

  const room = hosted.room.room;
  await b.call('match.endpoint', { room, port: 7002, lan: ['127.0.0.1'] });
  assert.equal((await a.next((m) => m.ev === 'match.peer')).seat, 1);
  assert.equal((await c.next((m) => m.ev === 'match.peer')).seat, 1);

  const token = hosted.relay_token;
  const toAll = encodeRelayFrame(token, 1, RELAY.broadcast, Uint8Array.from([1, 2, 3]));
  b.ws.send(toAll);
  const atA = (await a.next((m) => m.binary)).binary;
  assert.equal(atA[13], 1);
  assert.deepEqual(atA.subarray(16), Uint8Array.from([1, 2, 3]));
  assert.equal((await c.next((m) => m.binary)).binary[13], 1);
  c.ws.send(encodeRelayFrame(token, 2, 0, Uint8Array.from([9])));
  assert.deepEqual((await a.next((m) => m.binary)).binary.subarray(16), Uint8Array.from([9]));
  assert.equal((await b.call('ping')).code, 'ok');
  assert.ok(!b.messages.some((m) => m.binary));

  await a.call('room.leave', { room });
  assert.equal((await b.next((m) => m.ev === 'room.closed')).reason, 'host_left');
});

test('closing a socket makes the friend go offline for the other player', async () => {
  const a = await Socket.open();
  const b = await Socket.open();
  const ida = identity(31);
  const idb = identity(32);
  await a.hello(ida);
  await b.hello(idb);
  await a.call('friends.add', { code: idb.player });
  await b.call('friends.accept', { player: ida.player });
  a.ws.close();
  assert.equal((await b.next((m) => m.ev === 'friend.offline')).player, ida.player);
});
