import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { RELAY, encodeRelayFrame } from '../src/protocol.js';
import { makeHub, party } from './harness.js';

const SECRET = Uint8Array.from([0xc4, 0xfe, 0xba, 0xbe, 0x13, 0x37]);
const SECRET_HEX = 'c4febabe1337';
const frame = (token, dest, src = 0) => encodeRelayFrame(token, 1, dest, SECRET, src);
const counts = (clients) => clients.map((c) => c.binary.length);

function twoRooms() {
  const env = makeHub();
  const a = party(env, 4, 4, { first: 10 });
  const b = party(env, 3, 3, { first: 20 });
  return { env, a, b };
}

test('a frame from one room never reaches another', () => {
  const { a, b } = twoRooms();
  for (const client of a.clients) client.relay(frame(a.token, RELAY.broadcast));
  assert.deepEqual(counts(a.clients), [3, 3, 3, 3]);
  assert.deepEqual(counts(b.clients), [0, 0, 0]);
  for (const client of b.clients) client.relay(frame(b.token, RELAY.broadcast));
  assert.deepEqual(counts(a.clients), [3, 3, 3, 3]);
  assert.deepEqual(counts(b.clients), [2, 2, 2]);
});

test('seat 1 to seat 3 reaches seat 3 only, in that room only', () => {
  const { a, b } = twoRooms();
  a.clients[1].relay(frame(a.token, 3));
  assert.deepEqual(counts(a.clients), [0, 0, 0, 1]);
  assert.deepEqual(counts(b.clients), [0, 0, 0]);
  assert.equal(a.clients[3].binary[0][13], 1);
  assert.deepEqual(a.clients[3].binary[0].subarray(RELAY.headerBytes), SECRET);
});

test('a forged source seat is overwritten by the seat of the socket', () => {
  const { a } = twoRooms();
  for (const forged of [0, 2, 3, 0xfe]) a.clients[1].relay(frame(a.token, 3, forged));
  assert.deepEqual(a.clients[3].binary.map((f) => f[13]), [1, 1, 1, 1]);
});

test('a destination outside the room, empty or the sender itself is rejected', () => {
  const env = makeHub();
  const { clients, token } = party(env, 3, 4);
  for (const dest of [3, 4, 200, 1]) clients[1].relay(frame(token, dest));
  assert.deepEqual(counts(clients), [0, 0, 0]);
  assert.equal(env.logs.filter((l) => l.startsWith('relay drop reason=bad_destination')).length, 4);
});

test('a seat number of another room is meaningless: only the sender\'s room is routed', () => {
  const { a, b } = twoRooms();
  b.clients[1].relay(frame(b.token, 3));
  assert.deepEqual(counts(a.clients), [0, 0, 0, 0]);
  assert.deepEqual(counts(b.clients), [0, 0, 0]);
});

test('the token of another room is rejected, and its own token still works afterwards', () => {
  const { env, a, b } = twoRooms();
  b.clients[0].relay(frame(a.token, RELAY.broadcast));
  a.clients[0].relay(frame(b.token, 1));
  assert.deepEqual(counts(a.clients), [0, 0, 0, 0]);
  assert.deepEqual(counts(b.clients), [0, 0, 0]);
  assert.equal(env.logs.filter((l) => l.startsWith('relay drop reason=stale_token')).length, 2);
  a.clients[0].relay(frame(a.token, 1));
  assert.deepEqual(counts(a.clients), [0, 1, 0, 0]);
});

test('payloads are neither logged nor stored, and the Worker never reads past the header', () => {
  const { env, a, b } = twoRooms();
  for (let i = 0; i < 20; i++) {
    a.clients[1].relay(frame(a.token, RELAY.broadcast));
    b.clients[1].relay(frame(a.token, 0));
    b.clients[2].relay(frame(b.token, 0));
  }
  const logged = env.logs.join('\n');
  assert.ok(!logged.includes(SECRET_HEX));
  assert.ok(!logged.includes(String(SECRET)));
  assert.ok(!logged.includes(String(a.token)) && !logged.includes(String(b.token)));
  const tables = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`);
  for (const { name } of tables) {
    const rows = JSON.stringify(env.host.sql.all(`SELECT * FROM ${name}`));
    assert.ok(!rows.includes(SECRET_HEX) && !rows.includes('196,254'), name);
  }
  const hub = readFileSync(join(import.meta.dirname, '../src/hub.js'), 'utf8');
  assert.ok(!/relay\(conn, bytes\)[\s\S]*headerBytes/.test(hub));
});

test('budgets of one room do not touch another', () => {
  const { a, b } = twoRooms();
  const big = encodeRelayFrame(a.token, 1, RELAY.broadcast, new Uint8Array(RELAY.maxPayload));
  for (let i = 0; i < 2000; i++) a.clients[0].relay(big);
  assert.equal(a.clients[1].events('relay.closed').map((e) => e.reason)[0], 'rate_limit');
  b.clients[0].relay(frame(b.token, RELAY.broadcast));
  assert.deepEqual(counts(b.clients), [0, 1, 1]);
});
