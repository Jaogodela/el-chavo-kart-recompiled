import assert from 'node:assert/strict';
import { test } from 'node:test';

import { CAPABILITIES, PROTOCOL_VERSION, RELAY, RELAY_ROOM, encodeRelayFrame, parseRelayFrame } from '../src/protocol.js';
import { befriend, greeted, identity, makeHub, party } from './harness.js';

const bytes = (...values) => Uint8Array.from(values);
const ALL = RELAY.broadcast;
const frame = (token, dest, seq = 1, payload = bytes(7, 1, 2, 3, 4, 5, 6, 7, 8)) => encodeRelayFrame(token, seq, dest, payload);

const delivered = (clients) => clients.map((c) => c.binary.length);
const stamped = (original, src) => {
  const copy = Uint8Array.from(original);
  copy[13] = src;
  return copy;
};

test('hello answers with the service capabilities', () => {
  const env = makeHub();
  const reply = env.connect(identity(1)).hello();
  assert.deepEqual(reply.capabilities, CAPABILITIES);
  assert.deepEqual(CAPABILITIES, {
    protocol_version: PROTOCOL_VERSION, worker_build: CAPABILITIES.worker_build, public_endpoints: true, relay_supported: true,
    relay_max_payload: RELAY.maxPayload, min_players: 2, max_players: 4, four_player_rooms: true, seat_routing: true,
    race_telemetry: true,
  });
});

test('relay frames encode and decode; malformed ones say why', () => {
  const good = frame(0xdeadbeef, 2, 7, bytes(1, 2, 3));
  assert.deepEqual(parseRelayFrame(good), { type: RELAY.data, token: 0xdeadbeef, seq: 7, dest: 2, length: 3 });
  const bad = (edit) => {
    const copy = Uint8Array.from(good);
    edit(copy);
    return parseRelayFrame(copy).error;
  };
  assert.equal(bad((f) => (f[0] = 0x7b)), 'bad_magic');
  assert.equal(bad((f) => (f[2] = 9)), 'bad_version');
  assert.equal(bad((f) => (f[3] = 3)), 'bad_type');
  assert.equal(parseRelayFrame(good.subarray(0, 12)).error, 'truncated');
  assert.equal(parseRelayFrame(good.subarray(0, 3)).error, 'truncated');
  assert.equal(parseRelayFrame(good.subarray(0, good.length - 1)).error, 'bad_length');
  assert.equal(parseRelayFrame(frame(1, 0, 1, new Uint8Array(RELAY.maxPayload + 1))).error, 'oversized');
  assert.equal(parseRelayFrame(frame(1, 0, 1, new Uint8Array(0))).error, 'bad_length');
  assert.equal(parseRelayFrame(frame(1, ALL, 1, new Uint8Array(RELAY.maxPayload))).length, RELAY.maxPayload);
  assert.equal(parseRelayFrame('text').error, 'not_binary');
});

test('only relay-capable clients get a relay token, and it is the same token for every seat of the room', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3, { relay: false });
  befriend(a, b);
  befriend(a, c);
  const hosted = a.call('room.host', {});
  const found = (who) => who.call('room.search').rooms[0].handle;
  const joinedB = b.call('room.join', { handle: found(b) });
  assert.ok(hosted.relay_token > 0);
  assert.equal(joinedB.relay_token, hosted.relay_token);
});

test('a broadcast frame reaches every other seat of the room with the true sender seat, never the sender', () => {
  const env = makeHub();
  const { clients, token, room } = party(env, 4);
  const sent = frame(token, ALL, 5);
  clients[1].relay(sent);
  assert.deepEqual(delivered(clients), [1, 0, 1, 1]);
  assert.deepEqual(clients[0].binary[0], stamped(sent, 1));
  assert.deepEqual(clients[3].binary[0], stamped(sent, 1));
  assert.equal(parseRelayFrame(clients[2].binary[0]).dest, ALL);
  assert.ok(env.logs.includes(`relay forward room=${room} from=1 to=all bytes=${sent.length}`));
});

test('a frame names its destination seat; only that seat gets it, and the sender seat is the socket, not the frame', () => {
  const env = makeHub();
  const { clients, token } = party(env, 4);
  const forged = encodeRelayFrame(token, 9, 3, bytes(1, 2, 3), 0);
  clients[2].relay(forged);
  assert.deepEqual(delivered(clients), [0, 0, 0, 1]);
  assert.equal(clients[3].binary[0][13], 2);
  assert.deepEqual(clients[3].binary[0].subarray(16), bytes(1, 2, 3));
  const lie = encodeRelayFrame(token, 10, 0, bytes(4), 3);
  clients[1].relay(lie);
  assert.equal(clients[0].binary[0][13], 1);
});

test('destinations that are the sender, outside the room size or empty are dropped', () => {
  const env = makeHub();
  const { clients, token } = party(env, 2, 3);
  clients[0].relay(frame(token, 0));
  clients[0].relay(frame(token, 3));
  clients[0].relay(frame(token, 200));
  clients[0].relay(frame(token, 2));
  assert.deepEqual(delivered(clients), [0, 0]);
  assert.ok(env.logs.some((l) => l.startsWith('relay drop reason=bad_destination')));
});

test('frames never cross rooms: other rooms, even with the right token, receive nothing', () => {
  const env = makeHub();
  const one = party(env, 3, 3, { first: 10 });
  const two = party(env, 3, 4, { first: 20 });
  one.clients[1].relay(frame(one.token, ALL));
  assert.deepEqual(delivered(one.clients), [1, 0, 1]);
  assert.deepEqual(delivered(two.clients), [0, 0, 0]);
  two.clients[1].relay(frame(one.token, ALL));
  assert.deepEqual(delivered(one.clients), [1, 0, 1]);
  assert.deepEqual(delivered(two.clients), [0, 0, 0]);
  assert.ok(env.logs.some((l) => l.startsWith('relay drop reason=stale_token')));
});

test('a wrong token is dropped and the sender is told once', () => {
  const env = makeHub();
  const { clients, token } = party(env, 3);
  clients[1].relay(frame(token ^ 1, ALL));
  clients[1].relay(frame(token ^ 1, ALL));
  assert.deepEqual(delivered(clients), [0, 0, 0]);
  assert.deepEqual(clients[1].events('relay.closed').map((e) => e.reason), ['unavailable']);
  clients[1].relay(frame(token, ALL));
  assert.deepEqual(delivered(clients), [1, 0, 1]);
});

test('a player outside the room, or after leaving it, cannot inject even knowing the token', () => {
  const env = makeHub();
  const { clients, host, token, room } = party(env, 3);
  const outsider = greeted(env, 99);
  befriend(host, outsider);
  outsider.relay(frame(token, ALL));
  const stranger = env.connect(clients[0].who);
  stranger.relay(frame(token, ALL));
  clients[2].call('room.leave', { room });
  clients[2].relay(frame(token, ALL));
  assert.deepEqual(delivered(clients), [0, 0, 0]);
  assert.deepEqual(outsider.binary, []);
  assert.ok(env.logs.some((l) => l.startsWith('relay drop reason=no_relay')));
  assert.ok(env.logs.some((l) => l.startsWith('relay drop reason=unknown_connection')));
});

test('frames to a seat that is reconnecting or gone are dropped silently, and delivery resumes with the seat', () => {
  const env = makeHub();
  const { clients, token, room } = party(env, 3);
  clients[1].drop();
  clients[0].relay(frame(token, 1));
  clients[0].relay(frame(token, ALL));
  assert.deepEqual(delivered(clients), [0, 0, 1]);
  assert.deepEqual(clients[0].events('relay.closed'), []);
  const back = env.connect(clients[1].who);
  back.hello();
  assert.equal(back.call('match.resume', { room }).relay_token, token);
  clients[0].relay(frame(token, 1));
  clients[2].relay(frame(token, ALL));
  assert.equal(back.binary.length, 2);
  assert.equal(back.binary[1][13], 2);
  back.relay(frame(token, 0));
  assert.equal(clients[0].binary.length, 2);
});

test('a client that did not ask for relay at hello is not routed', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2, { relay: false });
  befriend(a, b);
  const token = a.call('room.host', {}).relay_token;
  const handle = b.call('room.search').rooms[0].handle;
  const joined = b.call('room.join', { handle });
  assert.equal(joined.relay_token, undefined);
  b.relay(frame(token, 0));
  a.relay(frame(token, 1));
  assert.deepEqual([a.binary.length, b.binary.length], [0, 0]);
});

test('the relay is not logged beyond the first few frame sizes, and never the payload', () => {
  const env = makeHub();
  const { clients, token } = party(env, 3);
  for (let i = 0; i < 10; i++) clients[0].relay(frame(token, ALL, i + 1));
  assert.equal(clients[1].binary.length, 10);
  assert.equal(env.logs.filter((l) => l.startsWith('relay forward')).length, RELAY.loggedForwards);
  assert.ok(!env.logs.some((l) => /payload|0x/.test(l)));
});

test('a sender that floods past its budget loses frames, and a persistent flood closes the room relay', () => {
  const env = makeHub();
  const { clients, token } = party(env, 4);
  const big = frame(token, ALL, 1, new Uint8Array(RELAY.maxPayload));
  for (let i = 0; i < 400; i++) clients[0].relay(big);
  const got = clients[1].binary.length;
  assert.ok(got > 0 && got < 400, `delivered ${got}`);
  for (let i = 0; i < RELAY.overrunsToClose + 10; i++) clients[0].relay(big);
  assert.deepEqual(clients[2].events('relay.closed').map((e) => e.reason), ['rate_limit']);
  const before = clients[1].binary.length;
  clients[3].relay(big);
  assert.equal(clients[1].binary.length, before);
});

test('a sender is held to one connection budget whatever its fan-out', () => {
  const env = makeHub();
  const a = party(env, 4, 4, { first: 10 });
  const b = party(env, 4, 4, { first: 20 });
  const payload = new Uint8Array(RELAY.maxPayload);
  const accepted = (p, dest) => {
    for (let i = 0; i < 400; i++) p.clients[0].relay(frame(p.token, dest, 1, payload));
    return p.clients[1].binary.length;
  };
  const allowed = Math.floor(RELAY.connection.bytes.burst / (payload.length + RELAY.headerBytes));
  assert.equal(accepted(a, ALL), allowed);
  assert.equal(accepted(b, 1), allowed);
});

test('the room budget is charged per delivery, so a room of broadcasters stops at the room limit', () => {
  const env = makeHub();
  const { clients, token } = party(env, 4);
  const payload = new Uint8Array(256);
  for (let i = 0; i < 700; i++) for (const c of clients) c.relay(frame(token, ALL, 1, payload));
  const deliveries = clients.reduce((sum, c) => sum + c.binary.length, 0);
  const { burst } = RELAY_ROOM[4].packets;
  assert.ok(deliveries <= burst && deliveries > burst - 4, `${deliveries} deliveries against a burst of ${burst}`);
});

test('the room budget grows with the room and a 2-seat room keeps the per-match reference', () => {
  assert.deepEqual(RELAY.connection, { packets: { burst: 800, perSecond: 400 }, bytes: { burst: 256 * 1024, perSecond: 128 * 1024 } });
  assert.deepEqual(RELAY_ROOM[2], { packets: { burst: 1600, perSecond: 800 }, bytes: { burst: 512 * 1024, perSecond: 256 * 1024 } });
  assert.deepEqual(RELAY_ROOM[3], { packets: { burst: 2400, perSecond: 1200 }, bytes: { burst: 768 * 1024, perSecond: 384 * 1024 } });
  assert.deepEqual(RELAY_ROOM[4], { packets: { burst: 3200, perSecond: 1600 }, bytes: { burst: 1024 * 1024, perSecond: 512 * 1024 } });
});

test('packet and byte budgets refill, and a reconnect does not refill them early', () => {
  const env = makeHub();
  const { clients, token, room } = party(env, 2);
  const small = frame(token, 1, 1, bytes(1));
  for (let i = 0; i < RELAY.connection.packets.burst + 10; i++) clients[0].relay(small);
  assert.equal(clients[1].binary.length, RELAY.connection.packets.burst);
  clients[0].drop();
  const back = env.connect(clients[0].who);
  back.hello();
  assert.equal(back.call('match.resume', { room }).code, 'ok');
  back.relay(small);
  assert.equal(clients[1].binary.length, RELAY.connection.packets.burst);
  env.clock.now += 1000;
  for (let i = 0; i < RELAY.connection.packets.perSecond + 10; i++) back.relay(small);
  assert.equal(clients[1].binary.length, RELAY.connection.packets.burst + RELAY.connection.packets.perSecond);
});

test('a slow seat loses frames without taking the others down, and only a seat that stays behind ends the relay', () => {
  const env = makeHub();
  const { clients, token } = party(env, 3);
  const [host, slow, fast] = clients;
  slow.buffered = RELAY.bufferedBytes + 1;
  for (let i = 0; i < 10; i++) host.relay(frame(token, ALL, i));
  assert.deepEqual([slow.binary.length, fast.binary.length], [0, 10]);
  assert.deepEqual(host.events('relay.closed'), []);
  slow.buffered = 0;
  host.relay(frame(token, ALL, 11));
  assert.deepEqual([slow.binary.length, fast.binary.length], [1, 11]);
  slow.buffered = RELAY.bufferedBytes + 1;
  for (let i = 0; i < 60; i++) {
    env.clock.now += 1000;
    for (let n = 0; n < 40; n++) host.relay(frame(token, 1, n));
    fast.relay(frame(token, 0, i));
  }
  assert.equal(fast.events('relay.closed')[0].reason, 'peer_backpressure');
  assert.equal(env.hub.relayBudgets.size, 0);
  assert.equal(env.hub.metrics.pending.get('relay_backpressure_drop'), RELAY.backpressureDropsToClose + 10);
});

test('a closed relay returns its seats to no transport, and the snapshot says so', () => {
  const env = makeHub();
  const { clients, host, room, token } = party(env, 3);
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'relay' }).code, 'ok');
  assert.equal(host.call('match.transport', { room, seat: 2, transport: 'direct' }).code, 'ok');
  const big = frame(token, ALL, 1, new Uint8Array(RELAY.maxPayload));
  for (let i = 0; i < 1000 && !clients[1].events('relay.closed').length; i++) clients[0].relay(big);
  assert.deepEqual(host.events('room.updated').at(-1).room.seats.map((s) => s.transport), ['', '', 'direct']);
  assert.equal(env.hub.roomRow(room).relay_token, 0);
});

test('a sender whose peers are away keeps its connection through the grace period and the resume', () => {
  const env = makeHub();
  const { clients, token, room } = party(env, 3);
  const [host, one, two] = clients;
  one.drop();
  two.drop();
  for (let round = 0; round < 20; round++) {
    for (let i = 0; i < 100; i++) {
      host.relay(frame(token, 1, i));
      host.relay(frame(token, ALL, i));
    }
    env.clock.now += 1000;
  }
  assert.equal(host.closedWith, null);
  assert.equal(env.hub.relayFaults.size, 0);
  const back = env.connect(one.who);
  back.hello();
  assert.equal(back.call('match.resume', { room }).code, 'ok');
  host.relay(frame(token, 1));
  assert.equal(back.binary.length, 1);
});

test('a relay that closed while a seat is away is not the sender\'s fault; once everyone is back it is', () => {
  const env = makeHub();
  const { clients, token, room } = party(env, 3);
  const [host, away] = clients;
  env.hub.endRelay(room, 'rate_limit');
  away.drop();
  for (let i = 0; i < RELAY.invalid.burst + 50; i++) host.relay(frame(token, ALL, i));
  assert.equal(host.closedWith, null);
  const back = env.connect(away.who);
  back.hello();
  back.call('match.resume', { room });
  for (let i = 0; i <= RELAY.invalid.burst; i++) host.relay(frame(token, ALL, i));
  assert.equal(host.closedWith?.code, 1008);
});

test('an occasional stray frame never closes a sender', () => {
  const env = makeHub();
  const { clients } = party(env, 2);
  for (let i = 0; i < 600; i++) {
    clients[0].relay(bytes(9));
    env.clock.now += 1000;
  }
  assert.equal(clients[0].closedWith, null);
});

test('a sender that keeps producing invalid frames is closed', () => {
  const env = makeHub();
  const { clients } = party(env, 2);
  for (let i = 0; i < 300 && !clients[0].closedWith; i++) clients[0].relay(bytes(1, 2, 3, 4));
  assert.equal(clients[0].closedWith.code, 1008);
});

test('relayed frames write nothing to the database', () => {
  const env = makeHub();
  const { clients, token } = party(env, 2);
  const run = env.host.sql.run;
  const writes = [];
  env.host.sql.run = (query, ...args) => {
    writes.push(query);
    return run(query, ...args);
  };
  for (let i = 0; i < 50; i++) clients[0].relay(frame(token, ALL, i, bytes(0xde, 0xad, 0xbe, 0xef)));
  assert.equal(clients[1].binary.length, 50);
  assert.deepEqual(writes, []);
  const tables = env.host.sql.all(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`).map((r) => r.name);
  assert.deepEqual(tables.sort(), ['friend_requests', 'friends', 'handles', 'invites', 'meta', 'metrics_daily', 'metrics_daily_character_pairs',
    'metrics_daily_characters', 'metrics_daily_compositions', 'metrics_daily_dimensions', 'metrics_daily_finish_positions',
    'metrics_daily_player_counts', 'metrics_daily_race_cross', 'metrics_daily_race_outcomes', 'metrics_samples', 'players', 'recent_rooms',
    'registrations', 'room_seats', 'rooms']);
});
