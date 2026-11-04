import assert from 'node:assert/strict';
import { test } from 'node:test';

import { OTHER_IMAGE, befriend, greeted, identity, joinRoom, makeHub, party } from './harness.js';

const seatsOf = (view) => view.seats.map((s) => s.player);

test('a 2-player room: host takes seat 0, a friend finds it by search and takes seat 1', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  const hosted = a.call('room.host', { capacity: 2, rules: { mode: 1, laps: 3 } });
  assert.equal(hosted.code, 'ok');
  assert.equal(hosted.seat, 0);
  assert.equal(hosted.room.state, 'open');
  assert.equal(hosted.room.capacity, 2);
  assert.deepEqual(hosted.room.rules, { mode: 1, laps: 3 });
  assert.equal(hosted.room.seats.length, 2);
  assert.deepEqual(hosted.room.seats.map((s) => s.state), ['connected', 'empty']);
  assert.ok(hosted.relay_token > 0);
  assert.equal(a.events('room.created')[0].room.room, hosted.room.room);

  const found = b.call('room.search');
  assert.equal(found.rooms.length, 1);
  assert.deepEqual([found.rooms[0].host, found.rooms[0].players, found.rooms[0].capacity], [a.who.player, 1, 2]);
  assert.ok(!('room' in found.rooms[0]) && !JSON.stringify(found).includes('ip'));
  const joined = b.call('room.join', { handle: found.rooms[0].handle });
  assert.equal(joined.code, 'ok');
  assert.equal(joined.seat, 1);
  assert.equal(joined.room.room, hosted.room.room);
  const update = a.events('room.updated').at(-1).room;
  assert.deepEqual(seatsOf(update), [a.who.player, b.who.player]);
  assert.equal(update.players, 2);
  assert.equal(b.call('friends.list').friends[0].state, 'hosting');
  assert.equal(a.call('friends.list').friends[0].state, 'connecting');
});

test('3 and 4 player rooms assign seats 1, 2, 3 in join order and reject the next join', () => {
  const env = makeHub();
  for (const size of [3, 4]) {
    const base = size * 10;
    const { clients, host } = party(env, size, size, { first: base });
    const replies = clients.slice(1);
    assert.equal(replies.length, size - 1);
    const latest = host.call('friends.list');
    assert.equal(latest.code, 'ok');
    const extra = greeted(env, base + 9);
    befriend(host, extra);
    assert.equal(joinRoom(host, extra).code, 'room_full', `${size} players`);
    const view = clients[1].events('room.updated');
    assert.equal(view.length, 0);
    const snapshot = host.call('room.update', { room: host.hub.roomOfPlayer(host.who.player).id });
    assert.deepEqual(snapshot.room.seats.map((s) => s.seat), [...Array(size).keys()]);
    assert.deepEqual(snapshot.room.seats.map((s) => s.player), clients.map((c) => c.who.player));
    for (const c of clients) c.call('room.leave', { room: snapshot.room.room });
  }
});

test('the 4 seat room refuses a fifth player and search shows it full', () => {
  const env = makeHub();
  const { host, clients } = party(env, 4);
  const fifth = greeted(env, 90);
  befriend(host, fifth);
  const found = fifth.call('room.search').rooms[0];
  assert.deepEqual([found.players, found.capacity], [4, 4]);
  assert.equal(fifth.call('room.join', { handle: found.handle }).code, 'room_full');
  assert.equal(clients.length, 4);
});

test('a seat sent by the client is never trusted', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3);
  befriend(a, b);
  befriend(a, c);
  a.call('room.host', { capacity: 4, seat: 3 });
  const handle = (who) => who.call('room.search').rooms[0].handle;
  assert.equal(b.call('room.join', { handle: handle(b), seat: 3 }).seat, 1);
  assert.equal(c.call('room.join', { handle: handle(c), seat: 0 }).seat, 2);
});

test('a handle is opaque, bound to the player who searched, and unknown handles are refused', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3);
  befriend(a, b);
  befriend(a, c);
  a.call('room.host', {});
  const handle = b.call('room.search').rooms[0].handle;
  assert.match(handle, /^[0-9a-f]{8}$/);
  assert.equal(c.call('room.join', { handle }).code, 'not_found');
  assert.equal(c.call('room.join', { handle: 'zzzz' }).code, 'not_found');
  assert.equal(c.call('room.join', {}).code, 'not_found');
  env.advance(121 * 1000);
  assert.equal(b.call('room.join', { handle }).code, 'not_found');
});

test('only friends see a room; strangers and non-friends cannot join it', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  a.call('room.host', {});
  assert.deepEqual(b.call('room.search').rooms, []);
  befriend(a, b);
  const handle = b.call('room.search').rooms[0].handle;
  a.call('friends.remove', { player: b.who.player });
  assert.equal(b.call('room.join', { handle }).code, 'not_friends');
});

test('a joiner leaving frees the seat and the next join takes the lowest free one', () => {
  const env = makeHub();
  const { host, clients, room } = party(env, 3, 4);
  const [, one, two] = clients;
  const late = greeted(env, 50);
  befriend(host, late);
  one.call('room.leave', { room });
  assert.deepEqual(seatsOf(host.events('room.updated').at(-1).room), [host.who.player, '', two.who.player, '']);
  assert.equal(host.call('friends.list').friends.find((f) => f.player === one.who.player).state, 'online');
  assert.equal(joinRoom(host, late).seat, 1);
  assert.equal(joinRoom(host, one).seat, 3);
  two.call('room.leave', { room });
  const again = greeted(env, 51);
  befriend(host, again);
  assert.equal(joinRoom(host, again).seat, 2);
});

test('the host leaving closes the room for everyone with a clear event', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 4);
  assert.equal(host.call('room.leave', { room }).code, 'ok');
  for (const g of guests) {
    const closed = g.events('room.closed');
    assert.deepEqual(closed.map((e) => [e.room, e.reason]), [[room, 'host_left']]);
    assert.equal(g.call('room.leave', { room }).code, 'ok');
  }
  assert.deepEqual(host.events('room.closed'), []);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM room_seats').n, 0);
  assert.equal(host.call('room.host', {}).code, 'ok');
  assert.equal(guests[0].call('room.host', {}).code, 'ok');
});

test('room.cancel closes the room for everyone', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3);
  assert.equal(guests[0].call('room.cancel', { room }).code, 'not_host');
  assert.equal(host.call('room.cancel', { room }).code, 'ok');
  assert.equal(guests[0].events('room.closed')[0].reason, 'cancelled');
  assert.equal(guests[1].events('room.closed')[0].reason, 'cancelled');
  assert.equal(host.call('room.cancel', { room }).code, 'not_found');
});

test('one room per player: hosting again refreshes it, a joiner or a started room cannot host', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 2);
  const again = host.call('room.host', {});
  assert.deepEqual([again.code, again.room.room, again.seat], ['ok', room, 0]);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
  assert.equal(guests[0].call('room.host', {}).code, 'busy');
  assert.equal(host.call('room.start', { room }).code, 'ok');
  assert.equal(host.call('room.host', {}).code, 'busy');
  host.call('room.leave', { room });
  const c = greeted(env, 8);
  befriend(guests[0], c);
  const d = greeted(env, 9);
  befriend(d, c);
  d.call('room.host', {});
  assert.equal(joinRoom(d, c).code, 'ok');
});

test('rooms are validated: capacity 2..4, bounded integer rules, presence required', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  for (const capacity of [1, 5, 0, 2.5, '3', null]) {
    env.advance(3000);
    assert.equal(a.call('room.host', { capacity }).code, capacity === null ? 'ok' : 'bad_request', String(capacity));
    a.call('room.cancel', { room: env.hub.roomOfPlayer(a.who.player)?.id });
  }
  const rules = (value) => {
    env.advance(3000);
    return a.call('room.host', { rules: value }).code;
  };
  assert.equal(rules({ Mode: 1 }), 'bad_request');
  assert.equal(rules({ mode: 'x' }), 'bad_request');
  assert.equal(rules({ mode: 70000 }), 'bad_request');
  assert.equal(rules([1]), 'bad_request');
  const many = Object.fromEntries(Array.from({ length: 17 }, (_, i) => [`k${String.fromCharCode(97 + i)}`, 1]));
  assert.equal(rules(many), 'bad_request');
  const probe = env.connect(identity(2), { presence: false });
  probe.hello();
  assert.equal(probe.call('room.host', {}).code, 'bad_request');
});

test('room.update: host only, rules and capacity, never below an occupied seat', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3, 4);
  assert.equal(guests[0].call('room.update', { room, rules: { mode: 2 } }).code, 'not_host');
  const updated = host.call('room.update', { room, rules: { mode: 2, track: 5 }, capacity: 3 });
  assert.equal(updated.code, 'ok');
  assert.equal(updated.room.capacity, 3);
  assert.deepEqual(updated.room.rules, { mode: 2, track: 5 });
  assert.equal(guests[1].events('room.updated').at(-1).room.rules.track, 5);
  assert.equal(host.call('room.update', { room, capacity: 2 }).code, 'capacity_too_small');
  assert.equal(host.call('room.update', { room, capacity: 9 }).code, 'bad_request');
  guests[0].call('room.leave', { room });
  assert.equal(host.call('room.update', { room, capacity: 2 }).code, 'capacity_too_small');
  assert.equal(host.call('room.update', { room, capacity: 3 }).code, 'ok');
});

test('start needs two players, freezes seats, rejects joins and moves to racing when all are ready', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  befriend(a, b);
  const { room } = a.call('room.host', { capacity: 3 }).room;
  assert.equal(a.call('room.start', { room }).code, 'need_players');
  const handle = b.call('room.search').rooms[0].handle;
  b.call('room.join', { handle });
  const c = greeted(env, 3);
  befriend(a, c);
  const handleC = c.call('room.search').rooms[0].handle;
  assert.equal(b.call('room.start', { room }).code, 'not_host');
  const started = a.call('room.start', { room });
  assert.equal(started.code, 'ok');
  assert.equal(started.room.state, 'starting');
  assert.equal(b.events('room.updated').at(-1).room.state, 'starting');
  assert.equal(c.call('room.join', { handle: handleC }).code, 'room_not_open');
  assert.deepEqual(c.call('room.search').rooms, []);
  assert.equal(a.call('room.start', { room }).code, 'room_not_open');
  assert.equal(a.call('room.update', { room, capacity: 4 }).code, 'room_not_open');
  assert.equal(a.call('friends.list').friends.length, 2);
  assert.equal(c.call('friends.list').friends[0].state, 'in_game');
  b.call('match.ready', { room, ready: true });
  assert.equal(b.events('room.updated').at(-1).room.state, 'starting');
  a.call('match.ready', { room, ready: true });
  const racing = b.events('room.updated').at(-1).room;
  assert.equal(racing.state, 'racing');
  assert.deepEqual(racing.seats.map((s) => s.ready), [true, true, false]);
});

test('seats stay frozen after the start: a leaver does not free the seat for a new player', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3, 3);
  const late = greeted(env, 60);
  befriend(host, late);
  const handle = late.call('room.search').rooms[0].handle;
  host.call('room.start', { room });
  host.inbox = [];
  guests[0].call('match.leave', { room });
  const view = host.events('room.updated').at(-1).room;
  assert.equal(view.seats[1].state, 'empty');
  assert.equal(view.state, 'starting');
  assert.equal(late.call('room.join', { handle }).code, 'room_not_open');
  guests[1].call('match.ready', { room, ready: true });
  host.call('match.ready', { room, ready: true });
  assert.equal(host.events('room.updated').at(-1).room.state, 'racing');
  host.call('room.leave', { room });
  assert.equal(guests[1].events('room.closed')[0].reason, 'host_left');
});

test('several rooms run side by side without leaking into each other', () => {
  const env = makeHub();
  const one = party(env, 3, 4, { first: 10 });
  const two = party(env, 2, 2, { first: 20 });
  assert.notEqual(one.room, two.room);
  one.host.call('room.update', { room: one.room, rules: { mode: 3 } });
  assert.deepEqual(two.clients.flatMap((c) => c.inbox), []);
  one.host.call('match.ready', { room: two.room, ready: true });
  assert.equal(one.host.call('match.ready', { room: two.room, ready: true }).code, 'not_found');
  assert.equal(two.guests[0].call('room.update', { room: one.room, rules: {} }).code, 'not_found');
  two.host.call('room.cancel', { room: two.room });
  assert.deepEqual(one.clients.flatMap((c) => c.events('room.closed')), []);
  assert.equal(one.guests[0].call('room.update', { room: one.room }).code, 'not_host');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
});

test('image and build must both match: incompatible friends neither see nor can be invited to a room', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const otherImage = greeted(env, 2, { image: OTHER_IMAGE });
  const otherBuild = greeted(env, 3, { build: 2 });
  const same = greeted(env, 4);
  for (const f of [otherImage, otherBuild, same]) befriend(a, f);
  a.call('room.host', { capacity: 4 });
  assert.deepEqual(otherImage.call('room.search').rooms, []);
  assert.deepEqual(otherBuild.call('room.search').rooms, []);
  assert.equal(same.call('room.search').rooms.length, 1);
  assert.equal(a.call('invite.send', { player: otherImage.who.player }).code, 'version_mismatch');
  assert.equal(a.call('invite.send', { player: otherBuild.who.player }).code, 'version_mismatch');
  const views = a.call('friends.list').friends;
  assert.deepEqual(views.map((v) => v.compatible), [false, false, true]);
  assert.deepEqual(views.map((v) => v.build), [1, 2, 1]);
});

test('hello refuses unknown images, games and protocol versions', () => {
  const env = makeHub();
  const hello = (extra) => env.connect(identity(1)).hello(extra).code;
  assert.equal(hello({ image: 'eck-usa-ffffffff' }), 'bad_image');
  assert.equal(hello({ image: undefined }), 'bad_image');
  assert.equal(hello({ game: 'other' }), 'bad_game');
  assert.equal(hello({ proto: 3 }), 'proto');
  assert.equal(hello({ build: -1 }), 'bad_request');
  assert.equal(hello({ build: 1.5 }), 'bad_request');
  assert.equal(hello({ player: 'AAAA-AAAA' }), 'unauthorized');
});

test('resume gives a player only their own held seat back; nobody can take it meanwhile', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 2, 3);
  const [b] = guests;
  b.drop();
  const held = host.events('room.updated').at(-1).room;
  assert.equal(held.seats[1].state, 'reconnecting');
  assert.equal(held.seats[1].player, b.who.player);

  const c = greeted(env, 3);
  befriend(host, c);
  const joined = joinRoom(host, c);
  assert.equal(joined.seat, 2);
  const d = greeted(env, 4);
  befriend(host, d);
  assert.equal(joinRoom(host, d).code, 'room_full');
  assert.equal(c.call('match.resume', { room }).code, 'bad_request');
  const e = greeted(env, 5);
  assert.equal(e.call('match.resume', { room }).code, 'not_found');
  c.call('room.leave', { room });
  assert.equal(e.call('match.resume', { room }).code, 'not_found');

  env.advance(5000);
  const back = env.connect(identity(2));
  assert.equal(back.hello().resume, room);
  const resumed = back.call('match.resume', { room });
  assert.equal(resumed.code, 'ok');
  assert.equal(resumed.seat, 1);
  assert.ok(resumed.relay_token > 0);
  assert.equal(host.events('room.updated').at(-1).room.seats[1].state, 'connected');
  assert.equal(back.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(back.call('match.resume', { room }).code, 'bad_request');
});

test('a held seat is freed after the hold; a host that does not return closes the room', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3, 3);
  guests[0].drop();
  env.advance(29 * 1000);
  assert.equal(host.events('room.updated').at(-1).room.seats[1].state, 'reconnecting');
  env.advance(2 * 1000);
  assert.equal(host.events('room.updated').at(-1).room.seats[1].state, 'empty');
  const late = env.connect(identity(2));
  assert.equal(late.hello().resume, null);
  assert.equal(late.call('match.resume', { room }).code, 'not_found');

  host.drop();
  assert.equal(guests[1].events('room.updated').at(-1).room.seats[0].state, 'reconnecting');
  env.advance(31 * 1000);
  assert.deepEqual(guests[1].events('room.closed').map((e) => e.reason), ['disconnect']);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 0);
});

test('resuming from a second connection takes the seat over from the stale one', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 2);
  const stale = guests[0];
  const fresh = env.connect(identity(2));
  fresh.hello();
  const reply = fresh.call('match.resume', { room });
  assert.equal(reply.code, 'ok');
  assert.equal(reply.seat, 1);
  assert.equal(stale.closedWith.code, 4001);
  assert.equal(fresh.call('match.ready', { room, ready: true }).code, 'ok');
  assert.equal(stale.call('match.ready', { room, ready: true }).code, 'not_found');
  assert.equal(host.events('room.updated').at(-1).room.seats[1].ready, true);
});

test('match.endpoint reaches every other seat tagged with the sender seat, in either publishing order', () => {
  const env = makeHub();
  const { clients, room } = party(env, 3);
  const [a, b, c] = clients;
  assert.equal(b.call('match.endpoint', { room, port: 7002, lan: ['192.168.1.5'] }).code, 'ok');
  const toA = a.events('match.peer');
  assert.equal(toA.length, 1);
  assert.deepEqual([toA[0].seat, toA[0].player, toA[0].port, toA[0].lan], [1, b.who.player, 7002, ['192.168.1.5']]);
  assert.equal(c.events('match.peer')[0].seat, 1);
  assert.equal(b.events('match.peer').length, 0);
  c.call('match.endpoint', { room, port: 7003, public_ip: '203.0.113.9', public_port: 50001,
    candidates: [{ ip: '203.0.113.9', port: 50001, kind: 'srflx' }] });
  assert.deepEqual(c.events('match.peer').map((e) => e.seat), [1]);
  const seenByB = b.events('match.peer');
  assert.deepEqual(seenByB.map((e) => e.seat), [2]);
  assert.deepEqual([seenByB[0].public_ip, seenByB[0].public_port, seenByB[0].candidates.length, seenByB[0].same_network],
    ['203.0.113.9', 50001, 1, true]);
  assert.equal(a.events('match.peer')[0].seat, 2);
  a.call('match.endpoint', { room, port: 7001 });
  assert.deepEqual([b, c].map((x) => x.events('match.peer').map((e) => e.seat)), [[0], [0]]);
  assert.deepEqual(a.events('match.peer').map((e) => e.seat).sort(), [1, 2]);
});

test('LAN candidates reach only a peer on the same public network, IPv6 by /64', () => {
  const env = makeHub({ local: false });
  const ip = { a: '203.0.113.10', b: '203.0.113.10', c: '198.51.100.20' };
  const a = greeted(env, 1, { ip: ip.a });
  const b = greeted(env, 2, { ip: ip.b });
  const c = greeted(env, 3, { ip: ip.c });
  befriend(a, b);
  befriend(a, c);
  const { room } = a.call('room.host', { capacity: 3 }).room;
  joinRoom(a, b);
  joinRoom(a, c);
  a.inbox = [];
  a.call('match.endpoint', { room, port: 7101, lan: ['192.168.1.5'], public_ip: ip.a, public_port: 50000, candidates: [
    { ip: '192.168.1.5', port: 7101, kind: 'host' }, { ip: ip.a, port: 50000, kind: 'srflx' }] });
  const near = b.events('match.peer')[0];
  assert.deepEqual([near.same_network, near.lan, near.candidates.length], [true, ['192.168.1.5'], 2]);
  const far = c.events('match.peer')[0];
  assert.deepEqual([far.same_network, far.lan, far.candidates.map((x) => x.ip)], [false, [], [ip.a]]);

  const v6 = makeHub({ local: false });
  const x = greeted(v6, 1, { ip: '2001:db8:1:2::5' });
  const y = greeted(v6, 2, { ip: '2001:0db8:0001:0002:aaaa::9' });
  befriend(x, y);
  const hosted = x.call('room.host', {}).room.room;
  joinRoom(x, y);
  x.call('match.endpoint', { room: hosted, port: 7101, lan: ['10.0.0.5'] });
  assert.deepEqual(y.events('match.peer').map((e) => [e.same_network, e.lan]), [[true, ['10.0.0.5']]]);
});

test('a client cannot claim somebody else\'s public address as its own', () => {
  const env = makeHub({ local: false });
  const a = greeted(env, 1, { ip: '203.0.113.10' });
  const b = greeted(env, 2, { ip: '198.51.100.20' });
  befriend(a, b);
  const { room } = a.call('room.host', {}).room;
  joinRoom(a, b);
  a.inbox = [];
  const victim = '192.0.2.77';
  assert.equal(a.call('match.endpoint', { room, port: 7101, public_ip: victim, public_port: 40000, candidates: [
    { ip: victim, port: 40000, kind: 'srflx' }, { ip: '203.0.113.10', port: 50000, kind: 'upnp' }] }).code, 'ok');
  const seen = b.events('match.peer')[0];
  assert.equal(seen.ip, '203.0.113.10');
  assert.equal(seen.public_ip, undefined);
  assert.equal(seen.public_port, undefined);
  assert.deepEqual(seen.candidates.map((c) => c.ip), ['203.0.113.10']);
  assert.ok(!JSON.stringify(seen).includes(victim));
  a.call('match.endpoint', { room, port: 7101, public_ip: '203.0.113.10', public_port: 40000 });
  assert.equal(b.events('match.peer')[0].public_port, 40000);
});

test('room.resume is match.resume under the room name', () => {
  const env = makeHub();
  const { guests, room } = party(env, 2);
  guests[0].drop();
  const back = env.connect(identity(2));
  back.hello();
  assert.equal(back.call('room.resume', { room }).seat, 1);
});

test('match.endpoint validates ports, addresses and candidates', () => {
  const env = makeHub();
  const { host, room } = party(env, 2);
  const bad = (extra) => host.call('match.endpoint', { room, port: 7001, ...extra }).code;
  assert.equal(bad({ port: 80 }), 'bad_request');
  assert.equal(bad({ lan: ['8.8.8.8'] }), 'bad_request');
  assert.equal(bad({ lan: ['10.0.0.1', '10.0.0.2', '10.0.0.3', '10.0.0.4', '10.0.0.5'] }), 'bad_request');
  assert.equal(bad({ public_ip: '10.0.0.1', public_port: 5 }), 'bad_request');
  assert.equal(bad({ public_ip: '203.0.113.9' }), 'bad_request');
  assert.equal(bad({ candidates: [{ ip: 'x', port: 5, kind: 'host' }] }), 'bad_request');
  assert.equal(bad({ candidates: [{ ip: '1.2.3.4', port: 5, kind: 'bogus' }] }), 'bad_request');
  assert.equal(bad({ candidates: Array(7).fill({ ip: '1.2.3.4', port: 5, kind: 'host' }) }), 'bad_request');
  assert.equal(host.call('match.endpoint', { room: 'nope', port: 7001 }).code, 'not_found');
  assert.equal(bad({}), 'ok');
});

test('match.transport: only the host reports, per peer, and relay needs both ends', () => {
  const env = makeHub();
  const { host, guests, room } = party(env, 3);
  assert.equal(guests[0].call('match.transport', { room, seat: 2, transport: 'relay' }).code, 'not_host');
  assert.equal(host.call('match.transport', { room, seat: 0, transport: 'direct' }).code, 'bad_request');
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'carrier' }).code, 'bad_request');
  assert.equal(host.call('match.transport', { room, seat: 1, transport: 'relay' }).code, 'ok');
  assert.deepEqual(guests[0].events('match.transport').map((e) => [e.seat, e.transport]), [[0, 'relay']]);
  assert.deepEqual(guests[1].events('match.transport'), []);
  assert.equal(host.call('match.transport', { room, seat: 2, transport: 'direct' }).code, 'ok');
  const seats = host.events('room.updated').at(-1).room.seats;
  assert.deepEqual(seats.map((s) => s.transport), ['', 'relay', 'direct']);

  const noRelay = greeted(env, 70, { relay: false });
  const other = party(env, 2, 2, { first: 80 });
  befriend(other.host, noRelay);
  joinRoom(other.host, noRelay);
  assert.equal(other.host.call('match.transport', { room: other.room, seat: 1, transport: 'relay' }).code, 'ok');
  const mixed = greeted(env, 90);
  const host2 = greeted(env, 91, { relay: false });
  host2.call('room.host', {});
  befriend(host2, mixed);
  joinRoom(host2, mixed);
  const room2 = env.hub.roomOfPlayer(host2.who.player).id;
  assert.equal(host2.call('match.transport', { room: room2, seat: 1, transport: 'relay' }).code, 'relay_unavailable');
});

test('friend presence follows the room: hosting, connecting, in_game, online again', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3);
  befriend(a, b);
  befriend(a, c);
  a.call('room.host', { capacity: 3 });
  const state = (e) => e.events('friend.status').at(-1)?.friend.state;
  assert.equal(state(b), 'hosting');
  assert.equal(state(c), 'hosting');
  joinRoom(a, b);
  assert.equal(state(a), 'connecting');
  assert.equal(state(c), 'hosting');
  const room = env.hub.roomOfPlayer(a.who.player).id;
  a.call('room.start', { room });
  assert.equal(state(c), 'in_game');
  a.call('room.leave', { room });
  assert.equal(state(c), 'online');
  b.drop();
  assert.equal(a.events('friend.offline').length, 1);
  assert.equal(c.events('friend.offline').length, 0);
});
