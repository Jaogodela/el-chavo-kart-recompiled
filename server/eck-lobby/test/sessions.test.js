// The invite-only flow: each host owns one hidden session of four seats, friends enter it by invite alone.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { befriend, greeted, makeHub } from './harness.js';

function hostSession(env, friends = 3, options = []) {
  const host = greeted(env, 1, options[0]);
  const guests = Array.from({ length: friends }, (_, i) => greeted(env, 2 + i, options[i + 1]));
  for (const guest of guests) befriend(host, guest);
  const hosted = host.call('room.host', { capacity: 4, rules: { laps: 3, items: 1 } });
  assert.equal(hosted.code, 'ok');
  host.inbox = [];
  env.advance(10_000);
  return { host, guests, room: hosted.room.room, token: hosted.relay_token };
}

const invite = (host, guest) => {
  const sent = host.call('invite.send', { player: guest.who.player });
  assert.equal(sent.code, 'ok');
  return guest.events('invite.received').at(-1).invite.id;
};

function joinAll(host, guests) {
  const ids = guests.map((guest) => invite(host, guest));
  return guests.map((guest, i) => guest.call('invite.accept', { invite: ids[i] }));
}

const publish = (client, room, port, ip) => client.call('match.endpoint', { room, port, public_ip: ip, public_port: port });

test('the host setup reaches a guest through the room rules', () => {
  const env = makeHub();
  const host = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(host, b);
  const rules = { setup: 1, game_type: 3, mode: 4, difficulty: 2 };
  const hosted = host.call('room.host', { capacity: 4, rules });
  assert.equal(hosted.code, 'ok');
  assert.equal(host.call('invite.send', { player: b.who.player }).code, 'ok');
  const received = b.events('invite.received').at(-1).invite;
  assert.deepEqual(received.rules, rules);
  assert.deepEqual(b.call('invite.accept', { invite: received.id }).room.rules, rules);
});

test('character picks: every seat sees them, and a seat only writes its own', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b, c]);
  const views = new Map();
  const seats = (client) => {
    const update = client.events('room.updated').at(-1);
    if (update) views.set(client, update.room);
    return views.get(client).seats;
  };

  assert.equal(host.call('match.pick', { room, pick: 2, character: 3 }).code, 'ok');
  assert.deepEqual([seats(b)[0].pick, seats(b)[0].character], [2, 3]);
  assert.equal(b.call('match.pick', { room, pick: 1, character: 5 }).code, 'ok');
  assert.deepEqual([seats(host)[1].pick, seats(host)[1].character], [1, 5]);
  assert.deepEqual([seats(c)[1].pick, seats(c)[1].character], [1, 5]);
  assert.deepEqual([seats(c)[0].pick, seats(c)[0].character], [2, 3]);
  assert.deepEqual([seats(c)[3].pick, seats(c)[3].character], [0, 0]);

  seats(host);
  assert.equal(b.call('match.pick', { room, pick: 1, character: 5 }).code, 'ok');
  assert.equal(host.events('room.updated').length, 0);

  assert.equal(b.call('match.pick', { room, pick: 2, character: 1, seat: 0 }).code, 'not_your_seat');
  assert.equal(b.call('match.pick', { room, pick: 2, character: 1, seat: 2 }).code, 'not_your_seat');
  assert.equal(b.call('match.pick', { room, pick: 2, character: 12 }).code, 'bad_request');
  assert.equal(b.call('match.pick', { room, pick: 2, character: -1 }).code, 'bad_request');
  assert.equal(b.call('match.pick', { room, pick: 2 }).code, 'bad_request');
  assert.equal(b.call('match.pick', { room, pick: 3, character: 1 }).code, 'bad_request');
  assert.equal(b.call('match.pick', { room, pick: '2', character: 1 }).code, 'bad_request');
  assert.deepEqual([seats(host)[0].pick, seats(host)[0].character], [2, 3]);
  assert.deepEqual([seats(host)[1].pick, seats(host)[1].character], [1, 5]);

  assert.equal(b.call('match.pick', { room, pick: 2, character: 1, seat: 1 }).code, 'ok');
  assert.equal(b.call('match.pick', { room, pick: 0 }).code, 'ok');
  assert.deepEqual([seats(host)[1].pick, seats(host)[1].character], [0, 0]);

  const stranger = greeted(env, 9);
  assert.equal(stranger.call('match.pick', { room, pick: 1, character: 1 }).code, 'not_found');
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.equal(b.call('match.pick', { room, pick: 1, character: 2 }).code, 'room_not_open');
});

test('after a race the host reopens the room: seats stay, and only the host can', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b, c]);
  assert.equal(host.call('room.reopen', { room }).code, 'ok', 'an open room: nothing to do');
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.equal(host.call('room.reopen', { room }).code, 'room_not_racing');
  for (const client of [host, b, c]) assert.equal(client.call('match.ready', { room, ready: true }).code, 'ok');
  for (const client of [host, b, c]) client.inbox = [];

  assert.equal(b.call('room.reopen', { room }).code, 'not_host');
  const rules = { setup: 1, game_type: 3, mode: 1, difficulty: 0, after_race: 1, after_dest: 2 };
  const reopened = host.call('room.reopen', { room, rules });
  assert.equal(reopened.code, 'ok');
  assert.equal(reopened.room.state, 'open');
  for (const client of [b, c]) {
    const view = client.events('room.updated').at(-1).room;
    assert.equal(view.state, 'open');
    assert.deepEqual(view.rules, rules);
    assert.equal(view.players, 3);
    assert.ok(view.seats.every((s) => !s.ready));
  }
  assert.equal(b.events('room.closed').length, 0);

  assert.equal(host.call('room.reopen', { room, rules }).code, 'ok', 'a repeat changes nothing');
  assert.equal(host.call('room.start', { room }).code, 'ok', 'and the next race can start');
  for (const client of [host, b, c]) assert.equal(client.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(b.events('room.updated').at(-1).room.state, 'racing');
});

test('the race reaches every seat, and all of them go on racing: nobody leaves', () => {
  const env = makeHub();
  const { host, guests: [b, c, d], room } = hostSession(env, 3);
  joinAll(host, [b, c, d]);
  const race = { setup: 1, game_type: 3, mode: 0, difficulty: 0, race: 1, race_mode: 0, race_track: 19, race_ai: 2, race_humans: 4,
    race_karts: 8, race_chars_low: 0x3210, race_chars_high: 0xb987, race_bots: 0xf0, race_teams: 0xaa, race_seats: 0o3210 };
  for (const client of [host, b, c, d]) client.inbox = [];

  assert.equal(host.call('room.update', { room, rules: race }).code, 'ok');
  assert.deepEqual(d.events('room.updated').at(-1).room.rules, race);
  assert.equal(b.call('room.update', { room, rules: {} }).code, 'not_host');
  assert.equal(b.call('room.start', { room }).code, 'not_host');
  assert.equal(host.call('room.start', { room }).code, 'ok');
  for (const client of [b, c, d]) assert.equal(client.events('room.updated').at(-1).room.state, 'starting');
  assert.equal(host.call('room.update', { room, rules: {} }).code, 'room_not_open');
  assert.equal(host.call('room.start', { room }).code, 'room_not_open');

  for (const client of [host, b, c]) assert.equal(client.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(d.events('room.updated').at(-1).room.state, 'starting');
  assert.equal(d.call('match.ready', { room, ready: true }).code, 'ok');
  for (const client of [host, b, c, d]) {
    const view = client.events('room.updated').at(-1).room;
    assert.equal(view.state, 'racing');
    assert.equal(view.players, 4);
    assert.deepEqual(view.rules, race);
    assert.equal(client.events('room.closed').length, 0);
  }
});

test('a real departure from a race is host_left for the host alone', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b, c]);
  assert.equal(host.call('room.start', { room }).code, 'ok');
  for (const client of [host, b, c]) client.call('match.ready', { room, ready: true });
  for (const client of [host, c]) client.inbox = [];
  assert.equal(b.call('room.leave', { room }).code, 'ok');
  assert.equal(host.events('room.closed').length, 0);
  assert.equal(host.events('room.updated').at(-1).room.players, 2);
  assert.equal(host.call('room.leave', { room }).code, 'ok');
  assert.equal(c.events('room.closed').at(-1).reason, 'host_left');
});

test('a seat that left leaves no pick behind for the next one', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b]);
  assert.equal(b.call('match.pick', { room, pick: 2, character: 8 }).code, 'ok');
  assert.equal(b.call('room.leave', { room }).code, 'ok');
  const [joined] = joinAll(host, [c]);
  assert.equal(joined.seat, 1);
  assert.deepEqual([joined.room.seats[1].pick, joined.room.seats[1].character], [0, 0]);
});

test('2 players: hidden session, invite, accept, same session, seat 1, endpoints both ways, direct, ready, racing', () => {
  const env = makeHub();
  const { host, guests: [b], room } = hostSession(env, 1);
  assert.equal(b.call('friends.list').friends[0].state, 'hosting');
  const [joined] = joinAll(host, [b]);
  assert.deepEqual([joined.code, joined.room.room, joined.seat], ['ok', room, 1]);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);

  publish(host, room, 7101, '203.0.113.1');
  publish(b, room, 7102, '203.0.113.2');
  assert.deepEqual(host.events('match.peer').map((e) => [e.seat, e.port]), [[1, 7102]]);
  assert.deepEqual(b.events('match.peer').map((e) => [e.seat, e.port]).at(-1), [0, 7101]);
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'direct' }).code, 'ok');
  assert.deepEqual(b.events('match.transport').map((e) => [e.seat, e.transport]), [[0, 'direct']]);

  assert.equal(host.call('room.start', { room }).code, 'ok');
  for (const c of [host, b]) assert.equal(c.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(host.events('room.updated').at(-1).room.state, 'racing');
});

test('3 players: each guest gets its own host path and never meets the other guest', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  assert.deepEqual(joinAll(host, [b, c]).map((r) => r.seat), [1, 2]);
  publish(host, room, 7101, '203.0.113.1');
  publish(b, room, 7102, '203.0.113.2');
  publish(c, room, 7103, '203.0.113.3');
  assert.deepEqual(host.events('match.peer').map((e) => e.seat).sort(), [1, 2]);
  assert.deepEqual([...new Set(b.events('match.peer').map((e) => e.seat))], [0]);
  assert.deepEqual([...new Set(c.events('match.peer').map((e) => e.seat))], [0]);
});

test('4 players: the third friend fills the session, a fifth player is refused, Invite stops', () => {
  const env = makeHub();
  const { host, guests: [b, c, d], room } = hostSession(env, 3);
  const fifth = greeted(env, 9);
  befriend(host, fifth);
  assert.deepEqual(joinAll(host, [b, c, d]).map((r) => r.seat), [1, 2, 3]);
  assert.equal(host.events('room.updated').at(-1).room.players, 4);
  assert.equal(host.call('invite.send', { player: fifth.who.player }).code, 'room_full');
  assert.equal(env.hub.seatRows(room).length, 4);
});

test('seats follow accept order, not invite order, and the service alone decides them', () => {
  const env = makeHub();
  const { host, guests: [b, c, d] } = hostSession(env, 3);
  const [ib, ic, id] = [b, c, d].map((guest) => invite(host, guest));
  assert.equal(c.call('invite.accept', { invite: ic, seat: 3 }).seat, 1);
  assert.equal(b.call('invite.accept', { invite: ib }).seat, 2);
  assert.equal(d.call('invite.accept', { invite: id }).seat, 3);
});

test('a friend on the Online menu with an empty session can be invited; accepting replaces it atomically', () => {
  const env = makeHub();
  const { host, guests: [b], room } = hostSession(env, 1);
  const own = b.call('room.host', { capacity: 4 }).room.room;
  const sent = invite(host, b);
  const joined = b.call('invite.accept', { invite: sent });
  assert.deepEqual([joined.code, joined.room.room, joined.seat], ['ok', room, 1]);
  assert.equal(env.hub.roomRow(own), null);
  assert.equal(b.events('room.closed').length, 0);
  assert.equal(b.call('match.endpoint', { room, port: 7102 }).code, 'ok');
});

test('a failed accept leaves the friend hosting their own empty session', () => {
  const env = makeHub();
  const { host, guests: [b], room } = hostSession(env, 1);
  const own = b.call('room.host', { capacity: 4 }).room.room;
  const sent = invite(host, b);
  env.host.sql.run(`UPDATE rooms SET state = 'starting' WHERE id = ?`, room);
  assert.equal(b.call('invite.accept', { invite: sent }).code, 'room_not_open');
  assert.equal(env.hub.roomOfPlayer(b.who.player).id, own);
});

test('a host with a guest is busy, and is free again once the guest leaves', () => {
  const env = makeHub();
  const { host, guests: [b], room } = hostSession(env, 1);
  const other = greeted(env, 8);
  befriend(other, host);
  other.call('room.host', { capacity: 4 });
  joinAll(host, [b]);
  assert.equal(other.call('invite.send', { player: host.who.player }).code, 'busy');
  assert.equal(other.call('friends.list').friends[0].state, 'connecting');
  b.call('room.leave', { room });
  assert.equal(other.events('friend.status').at(-1).friend.state, 'hosting');
  assert.equal(other.call('invite.send', { player: host.who.player }).code, 'ok');
});

test('joining one session ends the invites of every other session, and their hosts are told', () => {
  const env = makeHub();
  const { host, guests: [b] } = hostSession(env, 1);
  const other = greeted(env, 8);
  befriend(other, b);
  other.call('room.host', { capacity: 4 });
  const first = invite(host, b);
  const second = invite(other, b);
  b.call('invite.accept', { invite: first });
  assert.deepEqual(other.events('invite.ended').map((e) => [e.invite, e.reason]), [[second, 'target_busy']]);
  assert.deepEqual(b.events('invite.ended').map((e) => [e.invite, e.reason]), [[second, 'target_busy']]);
  assert.equal(b.call('invite.accept', { invite: second }).code, 'not_found');
});

test('invite end conditions: declined, expired, started, host left', () => {
  const env = makeHub();
  const { host, guests: [b, c, d], room } = hostSession(env, 3);
  const declined = invite(host, b);
  b.call('invite.decline', { invite: declined });
  assert.deepEqual(host.events('invite.ended').map((e) => e.reason), ['declined']);

  const expired = invite(host, b);
  env.advance(60_001);
  assert.deepEqual(b.events('invite.ended').map((e) => [e.invite, e.reason]), [[expired, 'expired']]);

  invite(host, c);
  invite(host, d);
  joinAll(host, [b]);
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.deepEqual(c.events('invite.ended').map((e) => e.reason), ['room_started']);
  assert.deepEqual(d.events('invite.ended').map((e) => e.reason), ['room_started']);

  const later = makeHub();
  const session = hostSession(later, 1);
  const left = invite(session.host, session.guests[0]);
  session.host.call('room.leave', { room: session.room });
  assert.deepEqual(session.guests[0].events('invite.ended').map((e) => [e.invite, e.reason]), [[left, 'host_left']]);
});

test('an incompatible friend cannot be invited and an offline one is refused', () => {
  const env = makeHub();
  const { host, guests: [b, c] } = hostSession(env, 2, [{}, { build: 2 }]);
  assert.equal(host.call('invite.send', { player: b.who.player }).code, 'version_mismatch');
  c.drop();
  assert.equal(host.call('invite.send', { player: c.who.player }).code, 'offline');
});

test('the start freezes the seats: a later accept is refused', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b]);
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.equal(host.call('invite.send', { player: c.who.player }).code, 'not_found');
  env.host.sql.run('INSERT INTO invites (id, room, sender, target, created) VALUES (?, ?, ?, ?, ?)', 'late', room, host.who.player,
    c.who.player, env.clock.now);
  assert.equal(c.call('invite.accept', { invite: 'late' }).code, 'room_not_open');
});

test('mixed transports: seat 1 direct, seat 2 relay, seat 3 direct, each decided alone by the host', () => {
  const env = makeHub();
  const { host, guests: [b, c, d], room, token } = hostSession(env, 3);
  joinAll(host, [b, c, d]);
  for (const [seat, transport] of [[1, 'direct'], [2, 'relay'], [3, 'direct']]) {
    assert.equal(host.call('match.transport', { room, seat, transport }).code, 'ok');
  }
  assert.deepEqual(b.events('match.transport').map((e) => e.transport), ['direct']);
  assert.deepEqual(c.events('match.transport').map((e) => e.transport), ['relay']);
  assert.deepEqual(d.events('match.transport').map((e) => e.transport), ['direct']);
  assert.ok(token > 0);
  const view = host.events('room.updated').at(-1).room;
  assert.deepEqual(view.seats.map((s) => s.transport), ['', 'direct', 'relay', 'direct']);
  assert.equal(env.hub.roomRow(room).used_direct, 1);
  assert.equal(env.hub.roomRow(room).used_relay, 1);
});

test('a guest without relay support cannot be put on the relay; another seat still can', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2, [{}, { relay: false }]);
  joinAll(host, [b, c]);
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'relay' }).code, 'relay_unavailable');
  assert.equal(host.call('match.transport', { room, seat: 2, transport: 'relay' }).code, 'ok');
});

test('reconnect: same identity, same seat, same session; the endpoint is published again and reaches the host', () => {
  const env = makeHub();
  const { host, guests: [b, c], room } = hostSession(env, 2);
  joinAll(host, [b, c]);
  publish(host, room, 7101, '203.0.113.1');
  b.drop();
  assert.deepEqual(host.events('room.updated').at(-1).room.seats.map((s) => s.state), ['connected', 'reconnecting', 'connected', 'empty']);
  env.advance(5_000);
  const back = env.connect(b.who, {});
  assert.equal(back.hello().resume, room);
  const resumed = back.call('match.resume', { room });
  assert.deepEqual([resumed.code, resumed.seat, resumed.room.room], ['ok', 1, room]);
  host.inbox = [];
  c.inbox = [];
  publish(back, room, 7102, '203.0.113.2');
  assert.deepEqual(host.events('match.peer').map((e) => e.seat), [1]);
  assert.equal(c.events('match.peer').length, 0);
});

test('a dropped guest that never returns frees its seat; a dropped host closes the session', () => {
  const env = makeHub();
  const { host, guests: [b, c] } = hostSession(env, 2);
  joinAll(host, [b, c]);
  b.drop();
  env.advance(31_000);
  assert.deepEqual(host.events('room.updated').at(-1).room.seats.map((s) => s.state), ['connected', 'empty', 'connected', 'empty']);
  host.drop();
  env.advance(31_000);
  assert.deepEqual(c.events('room.closed').map((e) => e.reason), ['disconnect']);
});
