import assert from 'node:assert/strict';
import { test } from 'node:test';

import { befriend, greeted, makeHub, party } from './harness.js';

function hostWithFriends(env, count, capacity = 4) {
  const host = greeted(env, 1);
  const friends = Array.from({ length: count }, (_, i) => greeted(env, 2 + i));
  for (const f of friends) befriend(host, f);
  const { room } = host.call('room.host', { capacity }).room;
  host.inbox = [];
  env.advance(10_000);
  return { host, friends, room };
}

const received = (client) => client.events('invite.received').at(-1)?.invite;

test('three friends are invited at once and accept independently: seats follow acceptance, assigned by the service', () => {
  const env = makeHub();
  const { host, friends, room } = hostWithFriends(env, 3);
  const [x, y, z] = friends;
  const ids = friends.map((f) => host.call('invite.send', { player: f.who.player }));
  assert.deepEqual(ids.map((r) => r.code), ['ok', 'ok', 'ok']);
  const invite = received(x);
  assert.equal(invite.id, ids[0].invite);
  assert.deepEqual([invite.host, invite.players, invite.capacity, invite.image], [host.who.player, 1, 4, 'eck-usa-1db4e96b']);
  assert.ok(invite.expires > env.clock.now);
  assert.ok(!('room' in invite));

  const third = z.call('invite.accept', { invite: ids[2].invite });
  const first = x.call('invite.accept', { invite: ids[0].invite });
  const second = y.call('invite.accept', { invite: ids[1].invite });
  assert.deepEqual([third.seat, first.seat, second.seat], [1, 2, 3]);
  for (const reply of [third, first, second]) assert.equal(reply.room.room, room);
  const view = host.events('room.updated').at(-1).room;
  assert.deepEqual(view.seats.map((s) => s.player), [host.who.player, z.who.player, x.who.player, y.who.player]);
  assert.equal(view.players, 4);
  assert.deepEqual(host.events('invite.ended').map((e) => [e.reason, e.room]), [['accepted', room], ['accepted', room], ['accepted', room]]);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
});

test('invite.accept joins the same room and never creates one', () => {
  const env = makeHub();
  const { host, friends, room } = hostWithFriends(env, 1);
  const sent = host.call('invite.send', { player: friends[0].who.player });
  const reply = friends[0].call('invite.accept', { invite: sent.invite });
  assert.equal(reply.room.room, room);
  assert.equal(reply.seat, 1);
  assert.equal(friends[0].events('room.created').length, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n, 1);
  assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'not_found');
});

test('outstanding invites never exceed the free seats, and a 4th invite to a 4-seat room is refused', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 4);
  for (const f of friends.slice(0, 3)) assert.equal(host.call('invite.send', { player: f.who.player }).code, 'ok');
  assert.equal(host.call('invite.send', { player: friends[3].who.player }).code, 'invite_limit');
  assert.equal(host.call('invite.send', { player: friends[0].who.player }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM invites').n, 3);
  const small = hostWithFriends(makeHub(), 2, 2);
  assert.equal(small.host.call('invite.send', { player: small.friends[0].who.player }).code, 'ok');
  assert.equal(small.host.call('invite.send', { player: small.friends[1].who.player }).code, 'invite_limit');
});

test('an invite to a full room is rejected, and invites that no longer fit end as room_full', () => {
  const env = makeHub();
  const { host, friends, room } = hostWithFriends(env, 4, 3);
  const [a, b, c, d] = friends;
  const ia = host.call('invite.send', { player: a.who.player });
  const ib = host.call('invite.send', { player: b.who.player });
  assert.equal(host.call('invite.send', { player: c.who.player }).code, 'invite_limit');
  assert.equal(a.call('invite.accept', { invite: ia.invite }).seat, 1);
  assert.equal(b.call('invite.accept', { invite: ib.invite }).seat, 2);
  assert.equal(host.call('invite.send', { player: c.who.player }).code, 'room_full');
  assert.equal(host.call('invite.send', { player: d.who.player }).code, 'room_full');
  host.call('room.update', { room, capacity: 4 });
  const ic = host.call('invite.send', { player: c.who.player });
  assert.equal(ic.code, 'ok');
  const id = host.call('invite.send', { player: d.who.player });
  assert.equal(id.code, 'invite_limit');
  assert.equal(c.call('invite.accept', { invite: ic.invite }).seat, 3);
});

test('a joiner by search fills the last seat: the remaining invite ends with room_full', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 3, 3);
  const [a, b] = friends;
  const ia = host.call('invite.send', { player: a.who.player });
  const ib = host.call('invite.send', { player: b.who.player });
  assert.equal(ib.code, 'ok');
  const handle = friends[2].call('room.search').rooms[0].handle;
  assert.equal(friends[2].call('room.join', { handle }).seat, 1);
  assert.deepEqual(b.events('invite.ended').map((e) => [e.invite, e.reason]), [[ib.invite, 'room_full']]);
  assert.deepEqual(a.events('invite.ended'), []);
  assert.equal(a.call('invite.accept', { invite: ia.invite }).seat, 2);
  assert.equal(b.call('invite.accept', { invite: ib.invite }).code, 'not_found');
});

test('decline tells the host; the decliner is not told about their own action', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 2);
  const sent = host.call('invite.send', { player: friends[0].who.player });
  assert.equal(friends[0].call('invite.decline', { invite: sent.invite }).code, 'ok');
  assert.deepEqual(host.events('invite.ended').map((e) => e.reason), ['declined']);
  assert.deepEqual(friends[0].events('invite.ended'), []);
  assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'not_found');
  assert.equal(friends[0].call('invite.decline', { invite: 'nope' }).code, 'ok');
  assert.equal(friends[1].call('invite.decline', { invite: sent.invite }).code, 'ok');
});

test('invites expire after 60 seconds for both sides', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 2);
  const early = host.call('invite.send', { player: friends[0].who.player });
  env.advance(30 * 1000);
  const late = host.call('invite.send', { player: friends[1].who.player });
  env.advance(31 * 1000);
  assert.deepEqual(friends[0].events('invite.ended').map((e) => [e.invite, e.reason]), [[early.invite, 'expired']]);
  assert.deepEqual(host.events('invite.ended').map((e) => [e.invite, e.reason]), [[early.invite, 'expired']]);
  assert.deepEqual(friends[1].events('invite.ended'), []);
  assert.equal(friends[0].call('invite.accept', { invite: early.invite }).code, 'not_found');
  assert.equal(friends[1].call('invite.accept', { invite: late.invite }).code, 'ok');
  env.advance(31 * 1000);
});

test('invites end when the host leaves, cancels or starts the race', () => {
  const env = makeHub();
  const one = hostWithFriends(env, 2);
  one.host.call('invite.send', { player: one.friends[0].who.player });
  one.host.call('room.leave', { room: one.room });
  assert.deepEqual(one.friends[0].events('invite.ended').map((e) => e.reason), ['host_left']);

  const two = party(env, 2, 4, { first: 10 });
  const extra = greeted(env, 20);
  befriend(two.host, extra);
  two.host.call('invite.send', { player: extra.who.player });
  assert.equal(two.host.call('room.start', { room: two.room }).code, 'ok');
  assert.deepEqual(extra.events('invite.ended').map((e) => e.reason), ['room_started']);

  const three = party(env, 2, 4, { first: 30 });
  const other = greeted(env, 40);
  befriend(three.host, other);
  three.host.call('invite.send', { player: other.who.player });
  three.host.call('room.cancel', { room: three.room });
  assert.deepEqual(other.events('invite.ended').map((e) => e.reason), ['cancelled']);
});

test('invites end as room_closed when the host drops and does not come back', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 1);
  const sent = host.call('invite.send', { player: friends[0].who.player });
  host.drop();
  env.advance(31 * 1000);
  assert.deepEqual(friends[0].events('invite.ended').map((e) => e.reason), ['room_closed']);
  assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'not_found');
});

test('only online, compatible friends who are free can be invited, only by the host of an open room', () => {
  const env = makeHub();
  const { host, friends, room } = hostWithFriends(env, 3);
  const [online, offline, busy] = friends;
  const stranger = greeted(env, 90);
  const rival = greeted(env, 91);
  befriend(rival, busy);
  rival.call('room.host', {});
  assert.equal(host.call('invite.send', { player: stranger.who.player }).code, 'not_friends');
  offline.drop();
  assert.equal(host.call('invite.send', { player: offline.who.player }).code, 'offline');
  assert.equal(host.call('invite.send', { player: busy.who.player }).code, 'ok');
  const joinRival = busy.call('room.search').rooms.find((r) => r.host === rival.who.player);
  assert.equal(busy.call('room.join', { handle: joinRival.handle }).code, 'ok');
  assert.equal(host.call('invite.send', { player: busy.who.player }).code, 'busy');
  assert.equal(busy.call('invite.accept', { invite: 'x' }).code, 'not_found');
  const sent = host.call('invite.send', { player: online.who.player });
  assert.equal(online.call('invite.accept', { invite: sent.invite }).code, 'ok');
  assert.equal(host.call('invite.send', { player: online.who.player }).code, 'already_in_room');
  assert.equal(online.call('invite.send', { player: busy.who.player }).code, 'not_found');
  assert.equal(host.call('invite.send', { player: host.who.player }).code, 'bad_request');
  assert.equal(room.length, 16);
});

test('accepting while already in another room is refused and the invite stays valid', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 1);
  const other = greeted(env, 70);
  befriend(other, friends[0]);
  const sent = host.call('invite.send', { player: friends[0].who.player });
  friends[0].call('room.host', {});
  assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'busy');
  assert.equal(friends[0].call('room.cancel', { room: env.hub.roomOfPlayer(friends[0].who.player).id }).code, 'ok');
  assert.equal(friends[0].call('invite.accept', { invite: sent.invite }).code, 'ok');
});

test('a target that goes offline drops its invites quietly', () => {
  const env = makeHub();
  const { host, friends } = hostWithFriends(env, 1);
  host.call('invite.send', { player: friends[0].who.player });
  friends[0].drop();
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM invites').n, 0);
  assert.deepEqual(host.events('invite.ended'), []);
});
