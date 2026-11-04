// In-process hub clients for the protocol tests: no sockets, a clock the test moves.
import { Hub } from '../src/hub.js';
import { GAME_ID, PROTOCOL_VERSION, SUPPORTED_IMAGES, playerIdFor } from '../src/protocol.js';
import { LocalConn, LocalHost, sqliteStore } from '../local/host.js';

export const IMAGE = SUPPORTED_IMAGES[0];
export const OTHER_IMAGE = 'eck-test-image';

export function identity(n) {
  const token = n.toString(16).padStart(64, 'a');
  return { token, player: playerIdFor(token) };
}

export function makeHub({ local = true, db } = {}) {
  const clock = { now: 1_700_000_000_000 };
  const logs = [];
  const host = new LocalHost(sqliteStore(db), { now: () => clock.now, log: (text) => logs.push(text) });
  const hub = new Hub(host, { local, images: [IMAGE, OTHER_IMAGE] });
  return {
    hub,
    host,
    clock,
    logs,
    advance(ms) {
      clock.now += ms;
      if (host.alarmAt !== null && host.alarmAt <= clock.now) {
        host.alarmAt = null;
        hub.alarm();
      }
    },
    connect(who, options) {
      return new Client(hub, host, who, options);
    },
  };
}

export class Client {
  constructor(hub, host, who, { ip = '127.0.0.1', country = '', image = IMAGE, build = 1, name, version = '0.1.0',
    platform = 'Windows', presence = true, relay = true } = {}) {
    this.hub = hub;
    this.host = host;
    this.who = who;
    this.fields = { image, build, name: name ?? `P${who.player.slice(0, 3)}`, version, platform, presence, relay };
    this.inbox = [];
    this.binary = [];
    this.closedWith = null;
    this.seq = 0;
    this.buffered = 0;
    const first = hub.admit(ip, country);
    this.admitted = Boolean(first);
    if (!first) return;
    const client = this;
    this.conn = new LocalConn({
      send: (text) => this.inbox.push(JSON.parse(text)),
      sendBinary: (bytes) => {
        this.binary.push(Uint8Array.from(bytes));
      },
      get bufferedAmount() {
        return client.buffered;
      },
      close: (code, reason) => {
        this.closedWith = { code, reason };
      },
    });
    this.conn.setAttachment(first);
    host.conns.add(this.conn);
  }

  raw(text) {
    const before = this.inbox.length;
    this.hub.message(this.conn, text);
    const replies = this.inbox.splice(before);
    const response = replies.find((m) => 're' in m);
    this.inbox.push(...replies.filter((m) => m !== response));
    return response;
  }

  relay(bytes) {
    this.hub.relay(this.conn, bytes);
  }

  call(op, body = {}) {
    return this.raw(JSON.stringify({ op, seq: ++this.seq, ...body }));
  }

  hello(extra = {}) {
    return this.call('hello', { proto: PROTOCOL_VERSION, game: GAME_ID, player: this.who.player, token: this.who.token,
      ...this.fields, ...extra });
  }

  events(name) {
    const taken = this.inbox.filter((m) => m.ev === name);
    this.inbox = this.inbox.filter((m) => m.ev !== name);
    return taken;
  }

  drop() {
    this.hub.closed(this.conn);
    this.host.conns.delete(this.conn);
  }
}

// The SQL writes from now on, one normalized line each.
export function countWrites(env) {
  const writes = [];
  const run = env.host.sql.run;
  env.host.sql.run = (query, ...args) => {
    if (/^\s*(INSERT|UPDATE|DELETE)/i.test(query)) writes.push(query.replace(/\s+/g, ' ').trim());
    return run(query, ...args);
  };
  return writes;
}

export const writesTo = (writes, table) => writes.filter((q) => q.includes(table));

export const abuse = (env) => Object.fromEntries(env.host.sql.all(
  `SELECT value, SUM(count) AS count FROM metrics_daily_dimensions WHERE dimension = 'abuse' GROUP BY value`)
  .map((r) => [r.value, r.count]));

export function greeted(env, n, options) {
  const client = env.connect(identity(n), options);
  assertOk(client.hello(), 'hello');
  return client;
}

function assertOk(reply, what) {
  if (reply?.code !== 'ok') throw new Error(`${what} answered ${reply?.code}`);
}

export function befriend(a, b) {
  a.call('friends.add', { code: b.who.player });
  b.call('friends.accept', { player: a.who.player });
  a.inbox = [];
  b.inbox = [];
}

// Searches as `joiner` and joins the room `host` hosts.
export function joinRoom(host, joiner) {
  const found = joiner.call('room.search').rooms.find((r) => r.host === host.who.player);
  if (!found) throw new Error('room not found by search');
  return joiner.call('room.join', { handle: found.handle });
}

// A host with a room of `capacity` seats and `size - 1` friends who joined it by search, in order. Inboxes are cleared.
export function party(env, size, capacity = size, { first = 1, options = {} } = {}) {
  const clients = Array.from({ length: size }, (_, i) => greeted(env, first + i, options));
  const [host, ...guests] = clients;
  const hosted = host.call('room.host', { capacity, rules: { mode: 1, track: 2 } });
  if (hosted.code !== 'ok') throw new Error(`room.host answered ${hosted.code}`);
  for (const guest of guests) {
    befriend(host, guest);
    assertOk(joinRoom(host, guest), 'join');
  }
  for (const c of clients) {
    c.inbox = [];
    c.binary = [];
  }
  return { clients, host, guests, room: hosted.room.room, token: hosted.relay_token };
}

// A party whose room has reached `racing` (every seat ready), the state race.start requires.
export function racingParty(env, size, capacity = size, options = {}) {
  const p = party(env, size, capacity, options);
  assertOk(p.host.call('room.start', { room: p.room }), 'room.start');
  for (const c of p.clients) assertOk(c.call('match.ready', { room: p.room, ready: true }), 'match.ready');
  for (const c of p.clients) c.inbox = [];
  return p;
}
