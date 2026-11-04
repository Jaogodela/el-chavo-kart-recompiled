import assert from 'node:assert/strict';
import { test } from 'node:test';

import { befriend, greeted, identity, makeHub } from './harness.js';

test('hello registers a player and answers with identity and capabilities', () => {
  const env = makeHub();
  const who = identity(1);
  const client = env.connect(who, { name: 'Chavo!!' });
  const reply = client.hello();
  assert.equal(reply.code, 'ok');
  assert.equal(reply.player, who.player);
  assert.equal(reply.friend_code, who.player);
  assert.equal(reply.name, 'Chavo');
  assert.equal(reply.resume, null);
  assert.equal(reply.capabilities.max_players, 4);
  assert.equal(client.hello().code, 'bad_request');
});

test('friend codes use letters without I and O and digits 2 to 9', () => {
  for (let n = 1; n < 200; n++) assert.match(identity(n).player, /^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$/);
});

test('a known player with another token is unauthorized; requests before hello say no_session', () => {
  const env = makeHub();
  greeted(env, 1);
  const thief = env.connect({ token: identity(2).token, player: identity(1).player });
  assert.equal(thief.hello().code, 'unauthorized');
  const early = env.connect(identity(3));
  assert.equal(early.call('friends.list').code, 'no_session');
  assert.equal(early.call('ping').code, 'no_session');
  assert.equal(early.call('nope').code, 'bad_request');
  assert.equal(early.raw('{').code, 'bad_request');
  assert.equal(early.raw(JSON.stringify({ op: 'ping' })).code, 'bad_request');
});

test('friends: request, accept, list with presence, remove', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  assert.equal(a.call('friends.add', { code: 'XXXX-XXXX' }).code, 'not_found');
  assert.equal(a.call('friends.add', { code: 'nope' }).code, 'bad_request');
  assert.equal(a.call('friends.add', { code: a.who.player }).code, 'bad_request');
  const added = a.call('friends.add', { code: b.who.player.toLowerCase().replace('-', ' ') });
  assert.equal(added.state, 'pending');
  assert.equal(b.events('friend.request')[0].player, a.who.player);
  assert.equal(b.call('friends.requests').incoming[0].player, a.who.player);
  assert.equal(a.call('friends.requests').outgoing[0].player, b.who.player);
  assert.equal(b.call('friends.accept', { player: a.who.player }).friend.state, 'online');
  assert.equal(a.events('friend.added')[0].friend.player, b.who.player);
  const view = a.call('friends.list').friends[0];
  assert.deepEqual([view.online, view.state, view.compatible, view.image, view.build], [true, 'online', true, 'eck-usa-1db4e96b', 1]);
  b.drop();
  assert.equal(a.events('friend.offline')[0].player, b.who.player);
  assert.deepEqual([a.call('friends.list').friends[0].online, a.call('friends.list').friends[0].state], [false, 'offline']);
  a.call('friends.remove', { player: b.who.player });
  assert.deepEqual(a.call('friends.list').friends, []);
  assert.equal(a.call('friends.decline', { player: b.who.player }).code, 'not_found');
});

test('adding someone who already asked makes you friends at once; declined requests vanish', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const b = greeted(env, 2);
  const c = greeted(env, 3);
  a.call('friends.add', { code: b.who.player });
  assert.equal(b.call('friends.add', { code: a.who.player }).state, 'friends');
  c.call('friends.add', { code: a.who.player });
  assert.equal(a.call('friends.decline', { player: c.who.player }).code, 'ok');
  assert.deepEqual(a.call('friends.requests').incoming, []);
});

test('presence false is a probe: not shown online and cannot host', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  const probe = env.connect(identity(2), { presence: false });
  assert.equal(probe.hello().code, 'ok');
  befriend(a, probe);
  assert.equal(a.call('friends.list').friends[0].online, false);
  assert.equal(probe.call('room.host', {}).code, 'bad_request');
  assert.equal(probe.call('room.search').code, 'ok');
});

test('the friend list is bounded', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  for (let i = 0; i < 200; i++) {
    env.host.sql.run('INSERT INTO friends (player, friend, since) VALUES (?, ?, 0)', a.who.player, `AAAA-${1000 + i}`);
  }
  const b = greeted(env, 2);
  assert.equal(b.call('friends.add', { code: a.who.player }).code, 'limit');
});

test('sockets, text and relay are separate: ping works, binary before hello is dropped', () => {
  const env = makeHub();
  const a = greeted(env, 1);
  assert.equal(a.call('ping').code, 'ok');
  const raw = env.connect(identity(2));
  raw.relay(Uint8Array.from([1, 2, 3]));
  assert.deepEqual(raw.binary, []);
});
