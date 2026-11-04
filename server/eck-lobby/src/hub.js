// The online hub: players, friends, presence, and the rooms (2 to 4 seats) friends gather in before a race.
// Signalling passes through it; race traffic runs directly between the PCs, or, when no direct path exists, as binary
// relay frames routed by seat between the sockets of one room. State lives in SQL tables and in each socket's
// attachment, so a hibernated Durable Object resumes as if it had never stopped; relay routes are a memory cache rebuilt
// from them. The same class runs in the development server (local/server.js).
import { PROTECTION, RateLimiter } from './abuse.js';
import { Metrics, SAMPLE_MS, countryCode, platformCategory, sampleBucket, versionCategory } from './metrics.js';
import {
  BUCKETS, CAPABILITIES, LIMITS, MAX_RACES_PER_ROOM, MIN_PLAYERS, ProtocolError, RACE_DURATION_GRACE_MS, RELAY, RELAY_ROOM,
  SUPPORTED_IMAGES, compatible, fail, ipGroup, ipv4Kind, isLocalAddress, normalizeFriendCode, ownEndpoint, parseCapacity,
  parseEndpoint, parseHello, parseRaceComplete, parseRaceStart, parseRelayFrame, parseRules,
} from './protocol.js';
import { sha256Hex, toHex } from './sha256.js';

const DAY_MS = 24 * 3600 * 1000;
const SOCIAL_OPS = new Set(['friends.add', 'room.host', 'room.join', 'invite.send', 'invite.accept', 'match.resume', 'room.resume']);
const GROWING_OPS = new Set(['hello', 'room.host', 'room.join', 'invite.accept', 'match.resume', 'room.resume']);
const REJECT_CODES = new Set(['proto', 'bad_game', 'bad_image', 'version_mismatch']);
const TOLD_MAX = 1024;
const ROOM_LIST_MAX = 50;

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS players (player TEXT PRIMARY KEY, token_hash TEXT NOT NULL, name TEXT NOT NULL,
     created INTEGER NOT NULL, seen INTEGER NOT NULL, session INTEGER NOT NULL DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS friends (player TEXT NOT NULL, friend TEXT NOT NULL, since INTEGER NOT NULL,
     PRIMARY KEY (player, friend))`,
  `CREATE TABLE IF NOT EXISTS friend_requests (sender TEXT NOT NULL, target TEXT NOT NULL, ms INTEGER NOT NULL,
     PRIMARY KEY (sender, target))`,
  `CREATE TABLE IF NOT EXISTS registrations (ip TEXT NOT NULL, ms INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS rooms (id TEXT PRIMARY KEY, host TEXT NOT NULL UNIQUE, capacity INTEGER NOT NULL,
     image TEXT NOT NULL, build INTEGER NOT NULL, rules TEXT NOT NULL, state TEXT NOT NULL,
     relay_token INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL, updated INTEGER NOT NULL, refreshed INTEGER NOT NULL,
     started INTEGER, established INTEGER, peak INTEGER NOT NULL DEFAULT 1, platforms TEXT NOT NULL DEFAULT '',
     used_direct INTEGER NOT NULL DEFAULT 0, used_relay INTEGER NOT NULL DEFAULT 0, races INTEGER NOT NULL DEFAULT 0,
     pending_race INTEGER, pending_started INTEGER, pending_players INTEGER, pending_competition TEXT,
     pending_track INTEGER, pending_characters TEXT)`,
  `CREATE TABLE IF NOT EXISTS room_seats (room TEXT NOT NULL, seat INTEGER NOT NULL, player TEXT NOT NULL UNIQUE, cid TEXT,
     grace INTEGER, ready INTEGER NOT NULL DEFAULT 0, transport TEXT NOT NULL DEFAULT '', counted INTEGER NOT NULL DEFAULT 0,
     relay INTEGER NOT NULL DEFAULT 0, platform TEXT NOT NULL DEFAULT '', version TEXT NOT NULL DEFAULT '',
     ip TEXT NOT NULL DEFAULT '', ep TEXT, PRIMARY KEY (room, seat))`,
  `CREATE TABLE IF NOT EXISTS invites (id TEXT PRIMARY KEY, room TEXT NOT NULL, sender TEXT NOT NULL,
     target TEXT NOT NULL, created INTEGER NOT NULL, UNIQUE (room, target))`,
  `CREATE TABLE IF NOT EXISTS handles (handle TEXT PRIMARY KEY, room TEXT NOT NULL, viewer TEXT NOT NULL,
     expires INTEGER NOT NULL, UNIQUE (room, viewer))`,
];

const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;

function takeToken(bucket, spec, now, cost = 1) {
  const [tokens, last] = bucket ?? [spec.burst, now];
  const refilled = Math.min(spec.burst, tokens + ((now - last) / 1000) * spec.perSecond);
  if (refilled < cost) return null;
  return [refilled - cost, now];
}

// Packets and bytes both have to fit; nothing is taken from either when one does not.
function takeRelayBudget(budget, spec, now, count, bytes) {
  const packets = takeToken(budget.packets, spec.packets, now, count);
  const volume = packets && takeToken(budget.bytes, spec.bytes, now, bytes);
  if (!volume) return false;
  budget.packets = packets;
  budget.bytes = volume;
  return true;
}

function relayToken() {
  return crypto.getRandomValues(new Uint32Array(1))[0] || 1;
}

function playerOf(value) {
  if (typeof value !== 'string' || !/^[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(value)) fail('bad_request', 'player expected');
  return value;
}

const idOf = (value) => (typeof value === 'string' ? value : '');

const seatTransport = (seat, room) =>
  (seat.transport === 'direct' ? 'direct' : seat.transport === 'relay' && room.relay_token ? 'relay' : 'pending');

const roomPeers = (seats) => seats.filter((s) => s.seat !== 0);

// The race a room has running, as the metrics take it.
const pendingRace = (room) => ({ competition: room.pending_competition, track: room.pending_track, playerCount: room.pending_players,
  characters: room.pending_characters ? JSON.parse(room.pending_characters) : null });

// What the seats ride on as the race ends: a seat that left no longer counts, and no peers at all is a pending race.
function raceTransport(room, seats) {
  const kinds = new Set(roomPeers(seats).map((s) => seatTransport(s, room)));
  if (kinds.has('direct') && kinds.has('relay')) return 'mixed';
  if (kinds.size !== 1) return 'pending';
  return [...kinds][0];
}

const clampedDuration = (reported, started, now) => Math.min(reported, Math.max(0, now - started) + RACE_DURATION_GRACE_MS);

const ROOM_RACE_COLUMNS = [['races', 'INTEGER NOT NULL DEFAULT 0'], ['pending_race', 'INTEGER'], ['pending_started', 'INTEGER'],
  ['pending_players', 'INTEGER'], ['pending_competition', 'TEXT'], ['pending_track', 'INTEGER'], ['pending_characters', 'TEXT']];

function ensureRoomRaceColumns(sql) {
  const have = new Set(sql.all('PRAGMA table_info(rooms)').map((c) => c.name));
  for (const [name, type] of ROOM_RACE_COLUMNS) {
    if (!have.has(name)) sql.run(`ALTER TABLE rooms ADD COLUMN ${name} ${type}`);
  }
}

// One room as the dashboard shows it: a short id and counts, no player, address, connection or token.
function roomReport(room, seats, now) {
  const peers = roomPeers(seats).map((s) => seatTransport(s, room));
  const tally = (kind) => peers.filter((t) => t === kind).length;
  return {
    id: room.id.slice(0, 6), state: room.state, capacity: room.capacity, occupied: seats.length,
    connected: seats.filter((s) => s.cid !== null).length, endpoints: seats.filter((s) => s.ep !== null).length,
    direct: tally('direct'), relay: tally('relay'), pending: tally('pending'), relay_available: room.relay_token !== 0,
    age_ms: Math.max(0, now - room.created),
  };
}

export class Hub {
  // host: { sql: { all(q, ...a), get(q, ...a), run(q, ...a) }, sockets(): Conn[], now(): ms, setAlarm(ms), log(text) }
  // options: { local, alarmAt, images } - alarmAt is the alarm the host already has stored, if any; images replaces
  // the supported game image list.
  // Conn: { send(text), sendBinary(bytes), close(code, reason), attachment(), setAttachment(object) }
  constructor(host, options = {}) {
    this.host = host;
    this.local = Boolean(options.local);
    this.images = options.images ?? SUPPORTED_IMAGES;
    // Never persisted: per room the relay route (seat -> socket) and budgets, per socket its fault state. Budgets outlive
    // a route, so reconnecting does not refill them.
    this.relayRooms = new Map();
    this.relayBudgets = new Map();
    this.relayDrops = new Map();
    this.relayTold = new Set();
    this.relayFaults = new Map();
    this.alarmAt = options.alarmAt ?? null;
    this.guards = {
      hello: new RateLimiter(PROTECTION.hello),
      hostAccount: new RateLimiter(PROTECTION.hostAccount),
      hostAddress: new RateLimiter(PROTECTION.hostAddress),
      joinAccount: new RateLimiter(PROTECTION.joinAccount),
      resumeAccount: new RateLimiter(PROTECTION.resumeAccount),
      inviteAccount: new RateLimiter(PROTECTION.inviteAccount),
      inviteAddress: new RateLimiter(PROTECTION.inviteAddress),
      raceAccount: new RateLimiter(PROTECTION.raceAccount),
      raceRoom: new RateLimiter(PROTECTION.raceRoom),
      rejects: new RateLimiter({ burst: 1, perSecond: 1000 / PROTECTION.rejectWindowMs }),
    };
    for (const statement of SCHEMA) host.sql.run(statement);
    ensureRoomRaceColumns(host.sql);
    this.metrics = new Metrics(host.sql, () => host.now());
    if (!this.meta('salt')) this.setMeta('salt', this.randomHex(16));
    this.reconcile();
  }

  // Seats whose socket is gone (the object or the server restarted) get the usual grace instead of staying occupied.
  reconcile() {
    const live = new Set(this.host.sockets().map((c) => c.attachment()?.cid).filter(Boolean));
    const until = this.host.now() + LIMITS.graceMs;
    for (const seat of this.host.sql.all('SELECT * FROM room_seats WHERE cid IS NOT NULL')) {
      if (!live.has(seat.cid)) {
        this.host.sql.run('UPDATE room_seats SET cid = NULL, grace = ?, ep = NULL WHERE room = ? AND seat = ?', until, seat.room, seat.seat);
      }
    }
    const present = new Set(this.host.sockets().map((c) => c.attachment()).filter((a) => a?.hello && a.presence).map((a) => a.player));
    for (const row of this.host.sql.all('SELECT * FROM invites')) {
      if (!present.has(row.target)) this.host.sql.run('DELETE FROM invites WHERE id = ?', row.id);
    }
    this.scheduleAlarm();
  }

  stats() {
    return {
      players: this.host.sql.get('SELECT COUNT(*) AS n FROM players').n,
      friendships: this.host.sql.get('SELECT COUNT(*) AS n FROM friends').n / 2,
      rooms: this.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n,
    };
  }

  // ---- connection lifecycle ----
  admit(ip, country = '') {
    const group = ipGroup(ip);
    if (!(this.local && isLocalAddress(ip))) {
      const open = this.host.sockets().filter((c) => ipGroup(c.attachment()?.ip) === group).length;
      if (open >= LIMITS.socketsPerIp) {
        this.diagnose('ws_upgrade_ip_cap');
        return null;
      }
    }
    this.diagnose('ws_upgrade_accepted');
    return { cid: this.randomHex(8), ip, opened: this.host.now(), country: countryCode(country) };
  }

  // Upgrades the Worker refused before they reached this object.
  edgeLimited(count) {
    if (count > 0) this.diagnose('ws_upgrade_limited', count);
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att) return;
    let seq = null;
    this.current = att;
    try {
      if (typeof text !== 'string') fail('bad_request', 'text frames only');
      if (text.length > LIMITS.frameBytes) fail('too_large');
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        fail('bad_request', 'invalid JSON');
      }
      if (!isObject(msg) || typeof msg.op !== 'string' || !isInt(msg.seq, 0, 0x7fffffff)) fail('bad_request', 'envelope');
      seq = msg.seq;
      const now = this.host.now();
      const any = takeToken(att.ra, BUCKETS.any, now);
      if (!any) fail('limit', 'slow down');
      att.ra = any;
      if (SOCIAL_OPS.has(msg.op)) {
        const social = takeToken(att.rs, BUCKETS.social, now);
        if (!social) fail('limit', 'slow down');
        att.rs = social;
      }
      if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', 'unknown op');
      if (msg.op !== 'hello' && !att.hello) fail('no_session');
      this.index = null;
      const body = HANDLERS[msg.op].call(this, att, msg, conn) ?? {};
      conn.setAttachment(att);
      conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
      this.runDeferred();
      if (GROWING_OPS.has(msg.op)) this.observe();
    } catch (error) {
      this.deferred = null;
      if (error instanceof ProtocolError && REJECT_CODES.has(error.code) && !att.rejected) {
        att.rejected = true;
        this.countReject(error.code, ipGroup(att.ip));
      }
      conn.setAttachment(att);
      if (!(error instanceof ProtocolError)) {
        this.host.log(`hub: internal error in request ${seq}: ${error?.stack ?? error}`);
        conn.send(JSON.stringify({ re: seq, code: 'internal' }));
      } else {
        conn.send(JSON.stringify({ re: seq, code: error.code }));
      }
    } finally {
      this.current = null;
      this.index = null;
    }
  }

  runDeferred() {
    while (this.deferred?.length) {
      const batch = this.deferred;
      this.deferred = null;
      for (const fn of batch) fn();
    }
  }

  // A socket closed: its seat is held for the grace period; presence ends with the player's last present socket.
  closed(conn) {
    const att = conn.attachment();
    if (att) this.forgetRelayConnection(att.cid);
    if (!att?.hello) return;
    this.index = null;
    if (att.localRace) this.track(() => this.metrics.raceAbandoned(att.localRace));
    att.localRace = null;
    if (att.room) {
      const seat = this.seatRow(att.room, att.seat);
      if (seat && seat.cid === att.cid) {
        this.host.sql.run('UPDATE room_seats SET cid = NULL, grace = ?, ep = NULL WHERE room = ? AND seat = ?',
          this.host.now() + LIMITS.graceMs, seat.room, seat.seat);
        this.forgetRelayRoom(seat.room);
        this.announce(seat.room);
        this.scheduleAlarm();
      }
    }
    att.room = null;
    att.seat = -1;
    att.hello = false;
    conn.setAttachment(att);
    this.index = null;
    if (att.presence && !this.presentSockets(att.player, att.cid).length) {
      this.host.sql.run('DELETE FROM invites WHERE target = ?', att.player);
      this.notifyFriends(att.player, { ev: 'friend.offline', player: att.player });
    }
    this.runDeferred();
    this.index = null;
  }

  alarm() {
    this.alarmAt = null;
    this.index = null;
    const now = this.host.now();
    this.host.sql.run('DELETE FROM handles WHERE expires <= ?', now);
    this.host.sql.run('DELETE FROM registrations WHERE ms < ?', now - DAY_MS);
    this.expireRequests();
    for (const row of this.host.sql.all('SELECT * FROM invites WHERE created + ? <= ?', LIMITS.inviteMs, now)) {
      this.endInvite(row, 'expired');
    }
    for (const seat of this.host.sql.all('SELECT * FROM room_seats WHERE grace IS NOT NULL AND grace <= ?', now)) {
      const room = this.roomRow(seat.room);
      const fresh = room && this.seatRow(seat.room, seat.seat);
      if (fresh && fresh.grace !== null && fresh.grace <= now) this.expireSeat(room, fresh);
    }
    for (const room of this.host.sql.all('SELECT * FROM rooms')) {
      if (room.created + LIMITS.roomMaxMs <= now || (room.state === 'open' && room.refreshed + LIMITS.openRoomMs <= now)) {
        this.closeRoom(room, 'expired');
      }
    }
    this.runDeferred();
    this.index = null;
    this.track(() => this.metrics.flushDiagnostics());
    this.observe();
  }

  // An alarm that is already set for the same moment or earlier stays: it fires early at worst, and alarm() sets the next.
  scheduleAlarm() {
    const times = [];
    const invited = this.host.sql.get('SELECT MIN(created) AS t FROM invites');
    if (invited?.t) times.push(invited.t + LIMITS.inviteMs);
    const grace = this.host.sql.get('SELECT MIN(grace) AS t FROM room_seats WHERE grace IS NOT NULL');
    if (grace?.t) times.push(grace.t);
    const open = this.host.sql.get(`SELECT MIN(refreshed) AS t FROM rooms WHERE state = 'open'`);
    if (open?.t) times.push(open.t + LIMITS.openRoomMs);
    const oldest = this.host.sql.get('SELECT MIN(created) AS t FROM rooms');
    if (oldest?.t) times.push(oldest.t + LIMITS.roomMaxMs);
    if (this.host.sockets().length || this.metrics.pending.size) times.push(sampleBucket(this.host.now()) + SAMPLE_MS);
    if (!times.length) return;
    const at = Math.min(...times);
    if (this.alarmAt !== null && this.alarmAt <= at) return;
    this.alarmAt = at;
    this.host.setAlarm(at);
  }

  // Metrics never fail a hub operation.
  track(fn) {
    try {
      fn();
    } catch (error) {
      this.host.log(`hub: metrics error: ${error?.stack ?? error}`);
    }
  }

  diagnose(name, amount = 1) {
    if (this.metrics.diagnose(name, amount)) this.scheduleAlarm();
  }

  // One rejection per reason and address per window: a reconnect loop must not write on every turn.
  countReject(reason, key) {
    if (this.guards.rejects.take(`${reason}|${key}`, this.host.now())) this.track(() => this.metrics.reject(reason));
  }

  exempt(att) {
    return this.local && isLocalAddress(att.ip);
  }

  observe() {
    this.track(() => this.metrics.observe(this.liveMetrics()));
    this.scheduleAlarm();
  }

  // What is online now, from the open sockets and the room tables: a player with several connections counts once.
  liveMetrics() {
    this.index = null;
    const sockets = this.sockets();
    const players = new Map();
    for (const { att } of sockets) if (att.presence && !players.has(att.player)) players.set(att.player, att);
    const tally = (pick) => {
      const counts = new Map();
      for (const att of players.values()) counts.set(pick(att), (counts.get(pick(att)) ?? 0) + 1);
      return counts;
    };
    const rooms = this.host.sql.all('SELECT * FROM rooms ORDER BY created DESC');
    const seatsOf = new Map(rooms.map((r) => [r.id, []]));
    for (const seat of this.host.sql.all('SELECT * FROM room_seats')) seatsOf.get(seat.room)?.push(seat);
    const transport = new Map();
    const bySize = new Map();
    let occupied = 0;
    let maxSeats = 0;
    for (const room of rooms) {
      const seats = seatsOf.get(room.id);
      occupied += seats.length;
      maxSeats += room.capacity;
      bySize.set(room.capacity, (bySize.get(room.capacity) ?? 0) + 1);
      for (const seat of roomPeers(seats)) {
        const kind = seatTransport(seat, room);
        transport.set(kind, (transport.get(kind) ?? 0) + 1);
      }
    }
    const now = this.host.now();
    return {
      online: players.size,
      in_rooms: occupied,
      rooms: rooms.length,
      advertised: rooms.filter((r) => r.state === 'open').length,
      active: rooms.filter((r) => r.state !== 'open').length,
      racing: rooms.filter((r) => r.pending_race !== null).length,
      connections: sockets.length,
      direct: transport.get('direct') ?? 0,
      relay: transport.get('relay') ?? 0,
      pending: transport.get('pending') ?? 0,
      rooms_2: bySize.get(2) ?? 0,
      rooms_3: bySize.get(3) ?? 0,
      rooms_4: bySize.get(4) ?? 0,
      occupied_seats: occupied,
      max_seats: maxSeats,
      average_occupancy: maxSeats ? Math.round(1000 * occupied / maxSeats) / 1000 : 0,
      platform: tally((att) => platformCategory(att.platform)),
      version: tally((att) => versionCategory(att.version)),
      country: tally((att) => countryCode(att.country)),
      transport,
      list: rooms.slice(0, ROOM_LIST_MAX).map((room) => roomReport(room, seatsOf.get(room.id), now)),
    };
  }

  metricsSummary() {
    return this.metrics.summary(this.liveMetrics());
  }

  metricsHistory(days, recent) {
    return this.metrics.history(days, recent);
  }

  // ---- helpers ----
  meta(key) {
    return this.host.sql.get('SELECT v FROM meta WHERE k = ?', key)?.v ?? null;
  }

  setMeta(key, value) {
    this.host.sql.run('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', key, String(value));
  }

  randomHex(bytes) {
    return toHex(crypto.getRandomValues(new Uint8Array(bytes)));
  }

  ipKey(ip) {
    return sha256Hex(`${this.meta('salt')}|${ipGroup(ip)}`).slice(0, 32);
  }

  // Greeted sockets; the one whose message is being handled carries its live attachment, not a stored copy.
  sockets() {
    if (!this.index) {
      this.index = [];
      for (const conn of this.host.sockets()) {
        const stored = conn.attachment();
        const att = stored && this.current && stored.cid === this.current.cid ? this.current : stored;
        if (att?.hello) this.index.push({ conn, att });
      }
    }
    return this.index;
  }

  socketByCid(cid) {
    return cid ? this.sockets().find((s) => s.att.cid === cid) ?? null : null;
  }

  presentSockets(player, exceptCid = null) {
    return this.sockets().filter((s) => s.att.player === player && s.att.presence && s.att.cid !== exceptCid);
  }

  send(conn, event) {
    try {
      conn.send(JSON.stringify(event));
    } catch {
      // A socket that is closing; its close handler runs next.
    }
  }

  later(fn) {
    (this.deferred ??= []).push(fn);
  }

  notifyPlayer(player, event) {
    for (const s of this.sockets()) if (s.att.player === player) this.send(s.conn, event);
  }

  // ---- players and presence ----
  playerRow(player) {
    return this.host.sql.get('SELECT * FROM players WHERE player = ?', player) ?? null;
  }

  stateOf(player) {
    const row = this.host.sql.get(`SELECT rooms.state AS state, rooms.host AS host FROM room_seats JOIN rooms ON rooms.id = room_seats.room
      WHERE room_seats.player = ?`, player);
    if (!row) return 'online';
    if (row.state === 'open') return row.host === player ? 'hosting' : 'connecting';
    return 'in_game';
  }

  friendView(player, viewer) {
    const row = this.playerRow(player);
    const view = { player, name: row?.name ?? '', online: false, state: 'offline' };
    const present = this.presentSockets(player);
    if (!present.length) return view;
    const att = present[0].att;
    return {
      ...view, online: true, state: this.stateOf(player), platform: att.platform, version: att.version, image: att.image,
      build: att.build, compatible: viewer ? compatible(viewer, att) : true,
    };
  }

  friendsOf(player) {
    return this.host.sql.all('SELECT friend FROM friends WHERE player = ? ORDER BY friend', player).map((r) => r.friend);
  }

  areFriends(a, b) {
    return Boolean(this.host.sql.get('SELECT 1 AS x FROM friends WHERE player = ? AND friend = ?', a, b));
  }

  notifyFriends(player, event) {
    for (const friend of this.friendsOf(player)) this.notifyPlayer(friend, event);
  }

  notifyStatus(player) {
    for (const friend of this.friendsOf(player)) {
      for (const s of this.presentSockets(friend)) {
        this.send(s.conn, { ev: 'friend.status', friend: this.friendView(player, s.att) });
      }
    }
  }

  statusLater(players) {
    this.later(() => {
      for (const player of players) this.notifyStatus(player);
    });
  }

  // ---- friends ----
  friendCount(player) {
    return this.host.sql.get('SELECT COUNT(*) AS n FROM friends WHERE player = ?', player).n;
  }

  expireRequests() {
    this.host.sql.run('DELETE FROM friend_requests WHERE ms < ?', this.host.now() - LIMITS.requestDays * DAY_MS);
  }

  makeFriends(a, b) {
    const now = this.host.now();
    this.host.sql.run('INSERT OR IGNORE INTO friends (player, friend, since) VALUES (?, ?, ?)', a, b, now);
    this.host.sql.run('INSERT OR IGNORE INTO friends (player, friend, since) VALUES (?, ?, ?)', b, a, now);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', a, b, b, a);
  }

  requireFriendRoom(a, b) {
    if (this.friendCount(a) >= LIMITS.friends || this.friendCount(b) >= LIMITS.friends) fail('limit', 'friend list full');
  }

  // Counted once the token matched, so nobody can lock out another player by guessing at the account.
  limitHello(att, player, now) {
    if (this.exempt(att) || this.guards.hello.take(player, now)) return;
    this.diagnose('hello_limited');
    fail('limit', 'too many sign-ins');
  }

  touchPlayer(row, name, now, newSession) {
    if (newSession) {
      this.host.sql.run('UPDATE players SET name = ?, seen = ?, session = ? WHERE player = ?', name, now, now, row.player);
    } else if (row.name !== name || now - row.seen >= PROTECTION.seenRefreshMs) {
      this.host.sql.run('UPDATE players SET name = ?, seen = ? WHERE player = ?', name, now, row.player);
    }
  }

  limitHost(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { hostAccount, hostAddress } = this.guards;
    if (!hostAccount.allows(att.player, now) || !hostAddress.allows(address, now)) {
      this.diagnose('room_host_limited');
      fail('limit', 'hosting too fast');
    }
    hostAccount.take(att.player, now);
    hostAddress.take(address, now);
  }

  limitJoin(att) {
    if (this.exempt(att) || this.guards.joinAccount.take(att.player, this.host.now())) return;
    this.diagnose('room_join_limited');
    fail('limit', 'joining too fast');
  }

  limitResume(att) {
    if (this.exempt(att) || this.guards.resumeAccount.take(att.player, this.host.now())) return;
    this.diagnose('room_resume_limited');
    fail('limit', 'resuming too fast');
  }

  limitInvite(att) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const address = ipGroup(att.ip);
    const { inviteAccount, inviteAddress } = this.guards;
    if (!inviteAccount.allows(att.player, now) || !inviteAddress.allows(address, now)) {
      this.diagnose('invite_limited');
      fail('limit', 'inviting too fast');
    }
    inviteAccount.take(att.player, now);
    inviteAddress.take(address, now);
  }

  // `roomKey`: the room, or the player for a race with no room.
  limitRace(att, roomKey) {
    if (this.exempt(att)) return;
    const now = this.host.now();
    const { raceAccount, raceRoom } = this.guards;
    if (!raceAccount.allows(att.player, now) || !raceRoom.allows(roomKey, now)) {
      this.diagnose('race_limited');
      fail('limit', 'race telemetry too fast');
    }
    raceAccount.take(att.player, now);
    raceRoom.take(roomKey, now);
  }

  // ---- rooms ----
  roomRow(id) {
    return id ? this.host.sql.get('SELECT * FROM rooms WHERE id = ?', id) ?? null : null;
  }

  roomOfPlayer(player) {
    return this.host.sql.get('SELECT rooms.* FROM room_seats JOIN rooms ON rooms.id = room_seats.room WHERE room_seats.player = ?', player) ?? null;
  }

  seatRows(id) {
    return this.host.sql.all('SELECT * FROM room_seats WHERE room = ? ORDER BY seat', id);
  }

  seatRow(room, seat) {
    return this.host.sql.get('SELECT * FROM room_seats WHERE room = ? AND seat = ?', room, seat) ?? null;
  }

  // The caller's own seat in the room msg.room names, bound to this very socket.
  mySeat(att, msg) {
    const room = this.roomRow(idOf(msg.room));
    const seat = room && att.room === room.id ? this.seatRow(room.id, att.seat) : null;
    if (!seat || seat.cid !== att.cid || seat.player !== att.player) fail('not_found');
    return { room, seat };
  }

  lowestFree(room, seats) {
    const taken = new Set(seats.map((s) => s.seat));
    for (let i = 0; i < room.capacity; i++) if (!taken.has(i)) return i;
    return -1;
  }

  view(room, seats = this.seatRows(room.id)) {
    const bySeat = new Map(seats.map((s) => [s.seat, s]));
    const list = [];
    for (let i = 0; i < room.capacity; i++) {
      const s = bySeat.get(i);
      list.push(s ? {
        seat: i, player: s.player, name: this.playerRow(s.player)?.name ?? '', state: s.cid ? 'connected' : 'reconnecting',
        ready: Boolean(s.ready), platform: s.platform, version: s.version, transport: s.transport,
      } : { seat: i, player: '', name: '', state: 'empty', ready: false, platform: '', version: '', transport: '' });
    }
    return {
      room: room.id, host: room.host, host_name: this.playerRow(room.host)?.name ?? '', capacity: room.capacity,
      players: seats.length, state: room.state, image: room.image, build: room.build, rules: JSON.parse(room.rules),
      seats: list, created: room.created, updated: room.updated,
    };
  }

  // Reply fields every entry into a room carries: the seat and, for relay-capable clients, the room's relay token.
  entry(room, seat, att) {
    return { room: this.view(room), seat, ...(room.relay_token && att.relay ? { relay_token: room.relay_token } : {}) };
  }

  sendToSeats(roomId, event, exceptCid = null) {
    for (const seat of this.seatRows(roomId)) {
      if (!seat.cid || seat.cid === exceptCid) continue;
      const s = this.socketByCid(seat.cid);
      if (s) this.send(s.conn, event);
    }
  }

  // Everyone seated gets the full snapshot, after the reply to the request that changed it.
  announce(id) {
    this.host.sql.run('UPDATE rooms SET updated = ? WHERE id = ?', this.host.now(), id);
    this.later(() => {
      const room = this.roomRow(id);
      if (room) this.sendToSeats(id, { ev: 'room.updated', room: this.view(room) });
    });
  }

  bindSeat(cid, room, seat) {
    const s = this.socketByCid(cid);
    if (!s) return;
    s.att.room = room;
    s.att.seat = seat;
    s.conn.setAttachment(s.att);
  }

  addSeat(room, seats, att) {
    const seat = this.lowestFree(room, seats);
    if (seat < 0) fail('room_full');
    this.host.sql.run(`INSERT INTO room_seats (room, seat, player, cid, relay, platform, version, ip) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
      room.id, seat, att.player, att.cid, att.relay ? 1 : 0, att.platform, att.version, att.ip);
    att.room = room.id;
    att.seat = seat;
    this.forgetRelayRoom(room.id);
    this.noteOccupant(room, seats.length + 1, platformCategory(att.platform));
    return seat;
  }

  // What the closed room reports: the fullest it was and the kinds of platform it held.
  noteOccupant(room, occupied, platform) {
    const known = room.platforms ? room.platforms.split(',') : [];
    const platforms = known.includes(platform) ? room.platforms : [...known, platform].sort().join(',');
    if (occupied > room.peak || platforms !== room.platforms) {
      this.host.sql.run('UPDATE rooms SET peak = ?, platforms = ? WHERE id = ?', Math.max(occupied, room.peak), platforms, room.id);
    }
  }

  // The service can tell how far a room got, not how its race ended.
  closedState(room, reason) {
    if (reason === 'disconnect') return 'Disconnected';
    if (reason === 'expired') return 'Timeout';
    if (room.established !== null) return 'Ended';
    return room.started !== null ? 'Failed' : 'Not started';
  }

  closeTelemetry(room, reason) {
    const now = this.host.now();
    const transport = room.used_relay ? (room.used_direct ? 'mixed' : 'relay') : room.used_direct ? 'direct' : 'pending';
    this.track(() => {
      this.metrics.roomClosed({ capacity: room.capacity, peak: room.peak, lifetimeMs: Math.max(0, now - room.created), transport });
      if (room.peak < MIN_PLAYERS) return;
      this.metrics.recordRoom({
        duration_ms: Math.max(0, now - (room.established ?? room.created)), capacity: room.capacity,
        peak: Math.min(room.peak, room.capacity), transport, platforms: room.platforms.split(',').join(' + '),
        state: this.closedState(room, reason),
      });
    });
    this.track(() => {
      if (room.pending_race !== null) this.metrics.raceAbandoned(pendingRace(room));
      this.metrics.roomRaceCount(room.races);
    });
  }

  // Capacity and rules of an open room; what did not change is not written, nor announced.
  applySettings(room, msg, { refresh }) {
    const capacity = msg.capacity === undefined ? room.capacity : parseCapacity(msg.capacity);
    const rules = msg.rules === undefined ? room.rules : JSON.stringify(parseRules(msg.rules));
    if (this.seatRows(room.id).some((s) => s.seat >= capacity)) fail('capacity_too_small');
    const now = this.host.now();
    if (capacity !== room.capacity || rules !== room.rules) {
      this.host.sql.run('UPDATE rooms SET capacity = ?, rules = ?, refreshed = ? WHERE id = ?', capacity, rules, now, room.id);
      this.forgetRelayRoom(room.id);
      this.trimInvites(this.roomRow(room.id));
      this.announce(room.id);
    } else if (refresh && now - room.refreshed >= PROTECTION.advertiseRefreshMs) {
      this.host.sql.run('UPDATE rooms SET refreshed = ? WHERE id = ?', now, room.id);
    }
  }

  hostConnected(room) {
    const seat = this.seatRow(room.id, 0);
    return Boolean(seat?.cid && this.socketByCid(seat.cid));
  }

  closeRoom(room, reason, actorCid = null) {
    const seats = this.seatRows(room.id);
    const inviteReason = reason === 'host_left' || reason === 'cancelled' ? reason : 'room_closed';
    for (const invite of this.host.sql.all('SELECT * FROM invites WHERE room = ?', room.id)) this.endInvite(invite, inviteReason);
    this.closeTelemetry(this.roomRow(room.id), reason);
    this.host.sql.run('DELETE FROM handles WHERE room = ?', room.id);
    this.host.sql.run('DELETE FROM room_seats WHERE room = ?', room.id);
    this.host.sql.run('DELETE FROM rooms WHERE id = ?', room.id);
    this.forgetRelayRoom(room.id);
    this.relayBudgets.delete(room.id);
    for (const seat of seats) {
      const s = this.socketByCid(seat.cid);
      if (s) {
        s.att.room = null;
        s.att.seat = -1;
        s.conn.setAttachment(s.att);
        if (s.att.cid !== actorCid) this.send(s.conn, { ev: 'room.closed', room: room.id, reason });
      }
    }
    this.statusLater(seats.map((s) => s.player));
  }

  // A seat leaves: the host takes the room with it, anyone else frees (open) or empties (frozen) their seat.
  removeSeat(room, seat, actorCid = null) {
    if (seat.seat === 0) {
      this.closeRoom(room, 'host_left', actorCid);
      return;
    }
    this.host.sql.run('DELETE FROM room_seats WHERE room = ? AND seat = ?', room.id, seat.seat);
    this.forgetRelayRoom(room.id);
    if (seat.cid) {
      const s = this.socketByCid(seat.cid);
      if (s) {
        s.att.room = null;
        s.att.seat = -1;
        s.conn.setAttachment(s.att);
      }
    }
    this.announce(room.id);
    this.statusLater([seat.player]);
    this.maybeRace(room.id);
  }

  expireSeat(room, seat) {
    if (seat.seat === 0) this.closeRoom(room, 'disconnect');
    else this.removeSeat(room, seat);
  }

  // Every seat of a starting room ready: the race is on.
  maybeRace(id) {
    const room = this.roomRow(id);
    if (!room || room.state !== 'starting') return;
    const seats = this.seatRows(id);
    if (seats.length < MIN_PLAYERS || !seats.every((s) => s.ready && s.cid)) return;
    this.host.sql.run(`UPDATE rooms SET state = 'racing', established = ? WHERE id = ?`, this.host.now(), id);
    this.announce(id);
  }

  endInvite(invite, reason) {
    this.host.sql.run('DELETE FROM invites WHERE id = ?', invite.id);
    if (reason !== 'declined' && reason !== 'accepted') {
      this.notifyPlayer(invite.target, { ev: 'invite.ended', invite: invite.id, reason });
    }
    if (reason === 'declined' || reason === 'accepted' || reason === 'expired') {
      this.notifyPlayer(invite.sender, { ev: 'invite.ended', invite: invite.id, room: invite.room, reason });
    }
  }

  // Outstanding invites never outnumber the free seats; the newest ones go first.
  trimInvites(room) {
    const free = room.capacity - this.seatRows(room.id).length;
    const invites = this.host.sql.all('SELECT * FROM invites WHERE room = ? ORDER BY created, rowid', room.id);
    for (const invite of invites.slice(Math.max(0, free))) this.endInvite(invite, 'room_full');
  }

  inviteView(invite, room) {
    const host = this.presentSockets(room.host)[0]?.att;
    return {
      id: invite.id, host: room.host, host_name: this.playerRow(room.host)?.name ?? '',
      players: this.seatRows(room.id).length, capacity: room.capacity, rules: JSON.parse(room.rules), image: room.image,
      build: room.build, platform: host?.platform ?? '', version: host?.version ?? '', expires: invite.created + LIMITS.inviteMs,
    };
  }

  // Common entry for room.join and invite.accept; `room` is the room the request resolved to.
  enter(att, room, { checkFriend }) {
    if (!att.presence) fail('bad_request', 'presence required');
    if (room.host === att.player) fail('bad_request', 'own room');
    if (checkFriend && !this.areFriends(att.player, room.host)) fail('not_friends');
    if (!this.hostConnected(room)) fail('offline');
    if (!compatible(att, room)) fail('version_mismatch');
    if (this.roomOfPlayer(att.player)) fail('busy');
    if (room.state !== 'open') fail('room_not_open');
    const seats = this.seatRows(room.id);
    if (seats.length >= room.capacity) fail('room_full');
    this.limitJoin(att);
    const seat = this.addSeat(room, seats, att);
    this.track(() => this.metrics.count('joins'));
    this.host.sql.run('DELETE FROM invites WHERE room = ? AND target = ?', room.id, att.player);
    this.host.sql.run('DELETE FROM handles WHERE viewer = ?', att.player);
    this.trimInvites(room);
    this.announce(room.id);
    this.statusLater([att.player, room.host]);
    return this.entry(this.roomRow(room.id), seat, att);
  }

  // The publisher's endpoint goes to every other connected seat, and it learns the endpoints already stored for them.
  introduce(roomId, publisherSeat) {
    const rows = this.seatRows(roomId);
    const mine = rows.find((s) => s.seat === publisherSeat);
    const eventFor = (from, to) => {
      const ep = JSON.parse(from.ep);
      const sameNetwork = ipGroup(from.ip) === ipGroup(to.ip);
      return {
        ev: 'match.peer', room: roomId, seat: from.seat, player: from.player, name: this.playerRow(from.player)?.name ?? '',
        ip: from.ip, port: ep.port, lan: sameNetwork ? ep.lan : [],
        ...(ep.publicPort ? { public_ip: ep.publicIp, public_port: ep.publicPort } : {}),
        candidates: sameNetwork ? ep.candidates : ep.candidates.filter((c) => ipv4Kind(c.ip) === 'public'),
        same_network: sameNetwork, relay: ep.relay,
      };
    };
    for (const other of rows) {
      if (other.seat === publisherSeat) continue;
      if (mine?.ep && other.cid) {
        const s = this.socketByCid(other.cid);
        if (s) this.send(s.conn, eventFor(mine, other));
      }
      if (other.ep && mine?.cid) {
        const s = this.socketByCid(mine.cid);
        if (s) this.send(s.conn, eventFor(other, mine));
      }
    }
  }

  // ---- relay ----
  // One binary frame: delivered to the seat(s) its header names inside the sender's own room, with the sender's seat
  // stamped by the service. Nothing in the frame can name another room.
  relay(conn, bytes) {
    const att = conn.attachment();
    if (!att) return this.dropRelay('unknown_connection');
    if (this.relayFaults.get(att.cid)?.abusive) return;
    if (!att.hello) return this.senderFault(conn, att, 'unknown_connection');
    const frame = parseRelayFrame(bytes);
    if (frame.error) return this.senderFault(conn, att, frame.error, att.room);
    this.index = null;
    const { route, fault } = this.relayRouteFor(att);
    this.index = null;
    if (!route) return this.senderFault(conn, att, 'no_relay', att.room, { tell: true, count: fault });
    if (frame.token !== route.token) return this.senderFault(conn, att, 'stale_token', route.room, { tell: true });
    const mine = att.seat;
    const targets = [];
    if (frame.dest === RELAY.broadcast) {
      for (const [seat, entry] of route.seats) if (seat !== mine) targets.push(entry);
    } else if (frame.dest === mine || !route.occupied.has(frame.dest)) {
      return this.senderFault(conn, att, 'bad_destination', route.room);
    } else if (route.seats.has(frame.dest)) {
      targets.push(route.seats.get(frame.dest));
    }
    if (!targets.length) return;
    const now = this.host.now();
    const budgets = this.relayBudget(route.room);
    const own = budgets.seats.get(mine) ?? {};
    budgets.seats.set(mine, own);
    const cost = bytes.length * targets.length;
    if (!takeRelayBudget(own, RELAY.connection, now, 1, bytes.length) ||
        !takeRelayBudget(budgets.room, RELAY_ROOM[route.capacity], now, targets.length, cost)) {
      this.dropRelay('rate_limit', route.room);
      if (++budgets.overruns >= RELAY.overrunsToClose) {
        this.diagnose('relay_abuse_closed');
        this.endRelay(route.room, 'rate_limit');
      }
      return;
    }
    const stamped = bytes.slice();
    stamped[13] = mine;
    for (const target of targets) {
      if (this.backedUp(target, route.room)) {
        if (target.backlog >= RELAY.backpressureDropsToClose) return this.endRelay(route.room, 'peer_backpressure');
        continue;
      }
      try {
        target.conn.sendBinary(stamped);
      } catch {
        this.endRelay(route.room, 'peer_unavailable');
        return;
      }
    }
    route.forwards += 1;
    if (route.forwards <= RELAY.loggedForwards) {
      this.host.log(`relay forward room=${route.room} from=${mine} to=${frame.dest === RELAY.broadcast ? 'all' : frame.dest} bytes=${bytes.length}`);
    }
  }

  // A slow seat loses frames instead of growing a queue; the others keep theirs. Only a seat that stays behind ends the relay.
  backedUp(target, room) {
    const buffered = target.conn.bufferedAmount;
    if (typeof buffered !== 'number' || buffered <= RELAY.bufferedBytes) {
      target.backlog = 0;
      return false;
    }
    target.backlog += 1;
    this.diagnose('relay_backpressure_drop');
    this.dropRelay('peer_backpressure', room);
    return true;
  }

  relayBudget(room) {
    let budgets = this.relayBudgets.get(room);
    if (!budgets) {
      budgets = { room: {}, seats: new Map(), overruns: 0 };
      this.relayBudgets.set(room, budgets);
    }
    return budgets;
  }

  // From the tables on a miss only; later frames take the cached sockets. Without a route, `fault` says whether the sender
  // is to blame: a seat that is away (grace, resuming) or a relay that closed with it is not the sender's doing.
  relayRouteFor(att) {
    if (!att.relay || !att.room) return { fault: true };
    const cached = this.relayRooms.get(att.room);
    if (cached && cached.seats.get(att.seat)?.cid === att.cid) return { route: cached };
    this.relayRooms.delete(att.room);
    const room = this.roomRow(att.room);
    const rows = room ? this.seatRows(room.id) : [];
    if (!rows.some((s) => s.seat === att.seat && s.cid === att.cid)) return { fault: true };
    if (!room.relay_token) return { fault: rows.every((s) => s.cid !== null) };
    const seats = new Map();
    for (const s of rows) {
      if (!s.cid || !s.relay) continue;
      const found = this.socketByCid(s.cid);
      if (found) seats.set(s.seat, { cid: s.cid, conn: found.conn, backlog: 0 });
    }
    const route = { room: room.id, token: room.relay_token, capacity: room.capacity, seats,
      occupied: new Set(rows.map((s) => s.seat)), forwards: 0 };
    this.relayRooms.set(room.id, route);
    return { route };
  }

  // The first few drops of each reason are logged, then one in a thousand. A sender whose room has no relay (any more)
  // hears it once per room.
  dropRelay(reason, room = null, att = null) {
    const count = (this.relayDrops.get(reason) ?? 0) + 1;
    this.relayDrops.set(reason, count);
    if (count <= 5 || count % 1000 === 0) this.host.log(`relay drop reason=${reason} room=${room ?? '-'} count=${count}`);
    if (!att?.room) return;
    const told = `${att.cid}:${att.room}`;
    if (this.relayTold.has(told)) return;
    if (this.relayTold.size >= TOLD_MAX) this.relayTold.clear();
    this.relayTold.add(told);
    this.index = null;
    const s = this.socketByCid(att.cid);
    this.index = null;
    if (s) this.send(s.conn, { ev: 'relay.closed', room: att.room, reason: 'unavailable' });
  }

  // Drops the frame; a sender that keeps producing frames no well-behaved client sends loses its connection.
  senderFault(conn, att, reason, room = null, { tell = false, count = true } = {}) {
    this.dropRelay(reason, room, tell ? att : null);
    if (!count) return;
    const state = this.relayFaults.get(att.cid) ?? { bucket: null, abusive: false };
    state.bucket = takeToken(state.bucket, RELAY.invalid, this.host.now());
    this.relayFaults.set(att.cid, state);
    if (state.bucket) return;
    state.abusive = true;
    this.host.log(`relay sender closed cid=${att.cid} reason=${reason}`);
    this.diagnose('relay_abuse_closed');
    try {
      conn.close(1008, 'relay abuse');
    } catch {
      // Already closing.
    }
  }

  endRelay(id, reason) {
    const room = this.roomRow(id);
    if (!room?.relay_token) return;
    this.host.sql.run('UPDATE rooms SET relay_token = 0 WHERE id = ?', id);
    this.host.sql.run(`UPDATE room_seats SET transport = '' WHERE room = ? AND transport = 'relay'`, id);
    this.forgetRelayRoom(id);
    this.relayBudgets.delete(id);
    this.host.log(`relay close room=${id} reason=${reason}`);
    this.index = null;
    this.sendToSeats(id, { ev: 'relay.closed', room: id, reason });
    this.announce(id);
    this.runDeferred();
    this.index = null;
  }

  forgetRelayRoom(id) {
    this.relayRooms.delete(id);
  }

  forgetRelayConnection(cid) {
    this.relayFaults.delete(cid);
    for (const [id, route] of this.relayRooms) {
      if ([...route.seats.values()].some((e) => e.cid === cid)) this.relayRooms.delete(id);
    }
  }
}

const HANDLERS = {
  hello(att, msg) {
    if (att.hello) fail('bad_request', 'already greeted');
    const hello = parseHello(msg, this.images);
    const now = this.host.now();
    const row = this.playerRow(hello.player);
    if (row && row.token_hash !== hello.hash) fail('unauthorized');
    this.limitHello(att, hello.player, now);
    let renamed = false;
    let newSession = hello.presence;
    if (!row) {
      const ipKey = this.ipKey(att.ip);
      if (!(this.local && isLocalAddress(att.ip))) {
        this.host.sql.run('DELETE FROM registrations WHERE ms < ?', now - DAY_MS);
        const recent = this.host.sql.get('SELECT COUNT(*) AS n FROM registrations WHERE ip = ?', ipKey).n;
        if (recent >= LIMITS.registrationsPerIpPerDay) fail('limit', 'too many new players');
      }
      this.host.sql.run('INSERT INTO players (player, token_hash, name, created, seen, session) VALUES (?, ?, ?, ?, ?, ?)',
        hello.player, hello.hash, hello.name, now, now, hello.presence ? now : 0);
      this.host.sql.run('INSERT INTO registrations (ip, ms) VALUES (?, ?)', ipKey, now);
    } else {
      renamed = row.name !== hello.name;
      newSession = hello.presence && now - row.session >= PROTECTION.sessionWindowMs;
      this.touchPlayer(row, hello.name, now, newSession);
    }
    const wasOnline = hello.presence && this.presentSockets(hello.player).length > 0;
    Object.assign(att, { hello: true, player: hello.player, name: hello.name, presence: hello.presence, image: hello.image,
      build: hello.build, version: hello.version, platform: hello.platform, relay: hello.relay, room: null, seat: -1 });
    if (newSession) this.track(() => this.metrics.session(att));
    else if (hello.presence) this.diagnose('hello_reconnect_deduped');
    this.diagnose('hello_accepted');
    this.index = null;
    if (hello.presence ? !wasOnline || renamed : renamed) this.later(() => this.notifyStatus(hello.player));
    const held = hello.presence ? this.host.sql.get('SELECT room FROM room_seats WHERE player = ? AND cid IS NULL', hello.player) : null;
    return { player: hello.player, name: hello.name, friend_code: hello.player, time: now, capabilities: CAPABILITIES,
      resume: held?.room ?? null };
  },

  ping() {
    return { time: this.host.now() };
  },

  'friends.list'(att) {
    return { friends: this.friendsOf(att.player).map((f) => this.friendView(f, att)) };
  },

  'friends.requests'(att) {
    this.expireRequests();
    const named = (rows, key) => rows.map((r) => ({ player: r[key], name: this.playerRow(r[key])?.name ?? '', ms: r.ms }));
    return {
      incoming: named(this.host.sql.all('SELECT sender, ms FROM friend_requests WHERE target = ? ORDER BY ms', att.player), 'sender'),
      outgoing: named(this.host.sql.all('SELECT target, ms FROM friend_requests WHERE sender = ? ORDER BY ms', att.player), 'target'),
    };
  },

  'friends.add'(att, msg) {
    const code = normalizeFriendCode(msg.code);
    const target = this.playerRow(code);
    if (!target) fail('not_found');
    if (target.player === att.player) fail('bad_request', 'own code');
    const result = { player: target.player, name: target.name };
    if (this.areFriends(att.player, target.player)) return { ...result, state: 'friends' };
    this.expireRequests();
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', target.player, att.player)) {
      this.requireFriendRoom(att.player, target.player);
      this.makeFriends(att.player, target.player);
      for (const s of this.presentSockets(target.player)) {
        this.send(s.conn, { ev: 'friend.added', friend: this.friendView(att.player, s.att) });
      }
      return { ...result, state: 'friends' };
    }
    if (this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', att.player, target.player)) {
      return { ...result, state: 'pending' };
    }
    const outgoing = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE sender = ?', att.player).n;
    const incoming = this.host.sql.get('SELECT COUNT(*) AS n FROM friend_requests WHERE target = ?', target.player).n;
    if (outgoing >= LIMITS.pendingOut || incoming >= LIMITS.pendingIn) fail('limit', 'too many pending requests');
    this.requireFriendRoom(att.player, target.player);
    this.host.sql.run('INSERT INTO friend_requests (sender, target, ms) VALUES (?, ?, ?)', att.player, target.player, this.host.now());
    this.notifyPlayer(target.player, { ev: 'friend.request', player: att.player, name: att.name });
    return { ...result, state: 'pending' };
  },

  'friends.accept'(att, msg) {
    const sender = playerOf(msg.player);
    this.expireRequests();
    if (!this.host.sql.get('SELECT 1 AS x FROM friend_requests WHERE sender = ? AND target = ?', sender, att.player)) fail('not_found');
    this.requireFriendRoom(att.player, sender);
    this.makeFriends(att.player, sender);
    for (const s of this.presentSockets(sender)) {
      this.send(s.conn, { ev: 'friend.added', friend: this.friendView(att.player, s.att) });
    }
    return { friend: this.friendView(sender, att) };
  },

  'friends.decline'(att, msg) {
    const other = playerOf(msg.player);
    const pair = [other, att.player, att.player, other];
    if (!this.host.sql.get(`SELECT 1 AS x FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)`, ...pair)) {
      fail('not_found');
    }
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', ...pair);
    return {};
  },

  'friends.remove'(att, msg) {
    const other = playerOf(msg.player);
    const were = this.areFriends(att.player, other);
    this.host.sql.run('DELETE FROM friends WHERE (player = ? AND friend = ?) OR (player = ? AND friend = ?)', att.player, other,
      other, att.player);
    this.host.sql.run('DELETE FROM friend_requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)', other,
      att.player, att.player, other);
    if (were) this.notifyPlayer(other, { ev: 'friend.removed', player: att.player });
    return {};
  },

  // Creates the sender's room, or, from the host of an open room, refreshes it: that is not a new room, so it costs no
  // creation budget, and an unchanged refresh inside the window writes nothing.
  'room.host'(att, msg, conn) {
    if (!att.presence) fail('bad_request', 'presence required');
    const existing = this.roomOfPlayer(att.player);
    if (existing) {
      if (existing.host !== att.player || existing.id !== att.room || existing.state !== 'open') fail('busy');
      this.applySettings(existing, msg, { refresh: true });
      return this.entry(this.roomRow(existing.id), 0, att);
    }
    const capacity = parseCapacity(msg.capacity);
    const rules = parseRules(msg.rules);
    this.limitHost(att);
    const now = this.host.now();
    const id = this.randomHex(8);
    this.host.sql.run(`INSERT INTO rooms (id, host, capacity, image, build, rules, state, relay_token, created, updated, refreshed)
      VALUES (?, ?, ?, ?, ?, ?, 'open', ?, ?, ?, ?)`, id, att.player, capacity, att.image, att.build, JSON.stringify(rules),
      att.relay ? relayToken() : 0, now, now, now);
    this.addSeat(this.roomRow(id), [], att);
    this.diagnose('room_host_accepted');
    this.track(() => this.metrics.roomCreated(capacity));
    const entry = this.entry(this.roomRow(id), 0, att);
    this.later(() => this.send(conn, { ev: 'room.created', ...entry }));
    this.statusLater([att.player]);
    this.scheduleAlarm();
    return entry;
  },

  'room.update'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    if (seat.seat !== 0) fail('not_host');
    if (room.state !== 'open') fail('room_not_open');
    this.applySettings(room, msg, { refresh: false });
    return { room: this.view(this.roomRow(room.id)) };
  },

  'room.cancel'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    if (seat.seat !== 0) fail('not_host');
    this.closeRoom(room, 'cancelled', att.cid);
    return {};
  },

  'room.leave'(att, msg) {
    const room = this.roomOfPlayer(att.player);
    if (!room || room.id !== msg.room) return {};
    const seat = this.host.sql.get('SELECT * FROM room_seats WHERE room = ? AND player = ?', room.id, att.player);
    this.removeSeat(room, seat, att.cid);
    att.room = null;
    att.seat = -1;
    return {};
  },

  // Handles are kept across searches while they have time left, so polling the list writes nothing.
  'room.search'(att) {
    const now = this.host.now();
    const held = new Map(this.host.sql.all('SELECT room, handle, expires FROM handles WHERE viewer = ?', att.player)
      .map((h) => [h.room, h]));
    const rooms = [];
    for (const room of this.host.sql.all(`SELECT rooms.* FROM rooms JOIN friends ON friends.friend = rooms.host
      WHERE friends.player = ? AND rooms.state = 'open' ORDER BY rooms.created`, att.player)) {
      if (!compatible(att, room) || !this.hostConnected(room)) continue;
      let handle = held.get(room.id)?.handle;
      if (!handle || held.get(room.id).expires - now < LIMITS.handleMs / 2) {
        handle = this.randomHex(4);
        this.host.sql.run('INSERT OR REPLACE INTO handles (handle, room, viewer, expires) VALUES (?, ?, ?, ?)', handle, room.id,
          att.player, now + LIMITS.handleMs);
      }
      const owner = this.presentSockets(room.host)[0]?.att;
      rooms.push({ handle, host: room.host, host_name: this.playerRow(room.host)?.name ?? '',
        players: this.seatRows(room.id).length, capacity: room.capacity, rules: JSON.parse(room.rules),
        platform: owner?.platform ?? '', version: owner?.version ?? '' });
      if (rooms.length >= LIMITS.searchResults) break;
    }
    return { rooms };
  },

  'room.join'(att, msg) {
    const handle = typeof msg.handle === 'string'
      ? this.host.sql.get('SELECT * FROM handles WHERE handle = ?', msg.handle) ?? null : null;
    const room = handle && handle.viewer === att.player && handle.expires > this.host.now() ? this.roomRow(handle.room) : null;
    if (!room) fail('not_found');
    return this.enter(att, room, { checkFriend: true });
  },

  'room.start'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    if (seat.seat !== 0) fail('not_host');
    if (room.state !== 'open') fail('room_not_open');
    if (this.seatRows(room.id).length < MIN_PLAYERS) fail('need_players');
    this.host.sql.run(`UPDATE rooms SET state = 'starting', started = ? WHERE id = ?`, this.host.now(), room.id);
    this.host.sql.run('UPDATE room_seats SET ready = 0 WHERE room = ?', room.id);
    for (const invite of this.host.sql.all('SELECT * FROM invites WHERE room = ?', room.id)) this.endInvite(invite, 'room_started');
    this.track(() => this.metrics.count('rooms_started'));
    this.announce(room.id);
    this.statusLater(this.seatRows(room.id).map((s) => s.player));
    return { room: this.view(this.roomRow(room.id)) };
  },

  'invite.send'(att, msg) {
    if (!att.presence) fail('bad_request', 'presence required');
    const target = playerOf(msg.player);
    const seat = att.room ? this.seatRow(att.room, att.seat) : null;
    const room = seat?.seat === 0 && seat.cid === att.cid ? this.roomRow(att.room) : null;
    if (!room || room.state !== 'open') fail('not_found');
    if (target === att.player) fail('bad_request', 'own code');
    if (!this.areFriends(att.player, target)) fail('not_friends');
    const recipient = this.presentSockets(target)[0];
    if (!recipient) fail('offline');
    if (!compatible(recipient.att, room)) fail('version_mismatch');
    const targetRoom = this.roomOfPlayer(target);
    if (targetRoom) fail(targetRoom.id === room.id ? 'already_in_room' : 'busy');
    const free = room.capacity - this.seatRows(room.id).length;
    if (free <= 0) fail('room_full');
    const others = this.host.sql.get('SELECT COUNT(*) AS n FROM invites WHERE room = ? AND target != ?', room.id, target).n;
    if (others >= Math.min(free, LIMITS.invitesPerRoom)) fail('invite_limit');
    this.limitInvite(att);
    const now = this.host.now();
    const id = this.randomHex(8);
    this.host.sql.run('DELETE FROM invites WHERE room = ? AND target = ?', room.id, target);
    this.host.sql.run('INSERT INTO invites (id, room, sender, target, created) VALUES (?, ?, ?, ?, ?)', id, room.id, att.player,
      target, now);
    this.track(() => this.metrics.count('invites'));
    this.scheduleAlarm();
    this.notifyPlayer(target, { ev: 'invite.received', invite: this.inviteView({ id, created: now }, room) });
    return { invite: id, expires: now + LIMITS.inviteMs };
  },

  'invite.accept'(att, msg) {
    const invite = typeof msg.invite === 'string'
      ? this.host.sql.get('SELECT * FROM invites WHERE id = ? AND target = ?', msg.invite, att.player) : null;
    const room = invite && this.roomRow(invite.room);
    if (!room) fail('not_found');
    const entry = this.enter(att, room, { checkFriend: false });
    this.endInvite(invite, 'accepted');
    return entry;
  },

  'invite.decline'(att, msg) {
    const invite = typeof msg.invite === 'string'
      ? this.host.sql.get('SELECT * FROM invites WHERE id = ? AND target = ?', msg.invite, att.player) : null;
    if (invite) this.endInvite(invite, 'declined');
    return {};
  },

  'match.endpoint'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    const endpoint = ownEndpoint(parseEndpoint(msg), att.ip);
    if (endpoint.relay && !att.relay) fail('bad_request', 'relay not signed in');
    const stored = JSON.stringify(endpoint);
    if (seat.ep !== stored || seat.ip !== att.ip) {
      this.host.sql.run('UPDATE room_seats SET ep = ?, ip = ? WHERE room = ? AND seat = ?', stored, att.ip, room.id, seat.seat);
    }
    this.later(() => this.introduce(room.id, seat.seat));
    return {};
  },

  'match.ready'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    if (typeof msg.ready !== 'boolean') fail('bad_request', 'ready expected');
    const ready = msg.ready ? 1 : 0;
    if (seat.ready === ready) return {};
    this.host.sql.run('UPDATE room_seats SET ready = ? WHERE room = ? AND seat = ?', ready, room.id, seat.seat);
    this.announce(room.id);
    this.maybeRace(room.id);
    return {};
  },

  // The host's choice for its path to one peer; a relay needs the room's relay and both ends signed in for it.
  'match.transport'(att, msg) {
    const { room, seat } = this.mySeat(att, msg);
    if (seat.seat !== 0) fail('not_host');
    if (!isInt(msg.seat, 1, room.capacity - 1)) fail('bad_request', 'seat expected');
    if (msg.transport !== 'direct' && msg.transport !== 'relay') fail('bad_request', 'transport expected');
    const peer = this.seatRow(room.id, msg.seat);
    if (!peer) fail('not_found');
    if (msg.transport === 'relay') {
      if (!room.relay_token || !seat.relay || !peer.relay || !peer.cid) fail('relay_unavailable');
      this.host.log(`relay transport selected room=${room.id} seat=${peer.seat}`);
    }
    if (peer.transport !== msg.transport) {
      this.host.sql.run('UPDATE room_seats SET transport = ?, counted = 1 WHERE room = ? AND seat = ?', msg.transport, room.id, peer.seat);
      const used = msg.transport === 'relay' ? 'used_relay' : 'used_direct';
      if (!room[used]) this.host.sql.run(`UPDATE rooms SET ${used} = 1 WHERE id = ?`, room.id);
      if (!peer.counted) this.track(() => this.metrics.transport(msg.transport));
      this.announce(room.id);
    }
    this.later(() => {
      const s = this.socketByCid(this.seatRow(room.id, peer.seat)?.cid);
      if (s) this.send(s.conn, { ev: 'match.transport', room: room.id, seat: 0, transport: msg.transport });
    });
    return {};
  },

  // Online: only the room's host reports, and the player count is the room's. No room: this PC's own race, solo or
  // split-screen, which needs no other PC to agree on it.
  'race.start'(att, msg) {
    const parsed = parseRaceStart(msg);
    if (msg.room !== undefined) {
      const { room, seat } = this.mySeat(att, msg);
      if (seat.seat !== 0) fail('not_host');
      if (room.state !== 'racing') fail('race_not_ready');
      if (parsed.race < room.races) return {};
      if (parsed.race > room.races) fail('race_order');
      if (room.races >= MAX_RACES_PER_ROOM) fail('limit', 'too many races in this room');
      const players = this.seatRows(room.id).length;
      if (parsed.players !== null && parsed.players !== players) fail('bad_request', 'players do not match the room');
      if (parsed.characters && parsed.characters.length !== players) fail('bad_request', 'characters do not match the room');
      this.limitRace(att, room.id);
      if (room.pending_race !== null) this.track(() => this.metrics.raceAbandoned(pendingRace(room)));
      this.host.sql.run(`UPDATE rooms SET races = races + 1, pending_race = ?, pending_started = ?, pending_players = ?,
        pending_competition = ?, pending_track = ?, pending_characters = ? WHERE id = ?`, parsed.race, this.host.now(), players,
      parsed.competition, parsed.track, parsed.characters && JSON.stringify(parsed.characters), room.id);
      this.track(() => this.metrics.raceStarted({ competition: parsed.competition, track: parsed.track, playerCount: players,
        characters: parsed.characters, online: true }));
      return {};
    }
    if (parsed.race <= (att.localLast ?? -1)) return {};
    const players = parsed.players ?? parsed.characters?.length ?? 1;
    if (parsed.characters && parsed.characters.length !== players) fail('bad_request', 'characters do not match players');
    this.limitRace(att, att.player);
    if (att.localRace) this.track(() => this.metrics.raceAbandoned(att.localRace));
    const race = { competition: parsed.competition, track: parsed.track, playerCount: players, characters: parsed.characters };
    att.localLast = parsed.race;
    att.localRace = { ...race, race: parsed.race, started: this.host.now() };
    this.track(() => this.metrics.raceStarted({ ...race, online: false }));
    return {};
  },

  'race.complete'(att, msg) {
    const parsed = parseRaceComplete(msg);
    if (msg.room !== undefined) {
      const { room, seat } = this.mySeat(att, msg);
      if (seat.seat !== 0) fail('not_host');
      if (room.pending_race === null) {
        if (parsed.race < room.races) return {};
        fail('race_mismatch');
      }
      if (parsed.race !== room.pending_race) fail('race_mismatch');
      const race = pendingRace(room);
      if (parsed.places && parsed.places.length !== race.playerCount) fail('bad_request', 'places do not match the race');
      this.limitRace(att, room.id);
      const durationMs = clampedDuration(parsed.durationMs, room.pending_started, this.host.now());
      const transport = raceTransport(room, this.seatRows(room.id));
      this.host.sql.run(`UPDATE rooms SET pending_race = NULL, pending_started = NULL, pending_players = NULL,
        pending_competition = NULL, pending_track = NULL, pending_characters = NULL WHERE id = ?`, room.id);
      this.track(() => this.metrics.raceCompleted({ ...race, durationMs, transport, places: parsed.places }));
      return {};
    }
    if (!att.localRace) {
      if (parsed.race <= (att.localLast ?? -1)) return {};
      fail('race_mismatch');
    }
    if (parsed.race !== att.localRace.race) fail('race_mismatch');
    if (parsed.places && parsed.places.length !== att.localRace.playerCount) fail('bad_request', 'places do not match the race');
    this.limitRace(att, att.player);
    const race = att.localRace;
    const durationMs = clampedDuration(parsed.durationMs, race.started, this.host.now());
    att.localRace = null;
    this.track(() => this.metrics.raceCompleted({ ...race, durationMs, places: parsed.places }));
    return {};
  },

  // Retakes the caller's own held seat after a lost connection; nobody else's.
  'match.resume'(att, msg) {
    if (att.room) fail('bad_request', 'already in a room');
    if (!att.presence) fail('bad_request', 'presence required');
    const room = this.roomRow(idOf(msg.room));
    const seat = room && this.host.sql.get('SELECT * FROM room_seats WHERE room = ? AND player = ?', room.id, att.player);
    if (!seat) fail('not_found');
    this.limitResume(att);
    const old = this.socketByCid(seat.cid);
    if (old && old.att.cid !== att.cid) {
      old.att.room = null;
      old.att.seat = -1;
      old.conn.setAttachment(old.att);
      try {
        old.conn.close(4001, 'seat taken by a new connection');
      } catch {
        // Already closing.
      }
    }
    this.host.sql.run(`UPDATE room_seats SET cid = ?, grace = NULL, relay = ?, platform = ?, version = ?, ip = ?, ep = NULL
      WHERE room = ? AND seat = ?`, att.cid, att.relay ? 1 : 0, att.platform, att.version, att.ip, room.id, seat.seat);
    att.room = room.id;
    att.seat = seat.seat;
    this.forgetRelayRoom(room.id);
    this.announce(room.id);
    this.statusLater([att.player]);
    this.later(() => this.introduce(room.id, seat.seat));
    return this.entry(room, seat.seat, att);
  },
};

HANDLERS['match.leave'] = HANDLERS['room.leave'];
HANDLERS['room.resume'] = HANDLERS['match.resume'];
