import { VALID_CHARACTERS, VALID_COMPETITIONS, VALID_TRACKS } from './game_data.js';
import { sha256Hex } from './sha256.js';

export const SERVICE = 'eck-lobby';
export const PROTOCOL_VERSION = 1;
export const GAME_ID = 'el-chavo-kart-pc';
export const SUPPORTED_IMAGES = ['eck-usa-1db4e96b'];
export const WORKER_BUILD = '2026-11-04-production3';

export const MIN_PLAYERS = 2;
export const MAX_PLAYERS = 4;

export const MAX_RACES_PER_ROOM = 64;
export const RACE_DURATION = { minMs: 1000, maxMs: 60 * 60 * 1000 };
export const RACE_DURATION_GRACE_MS = 5000;

export const LIMITS = {
  frameBytes: 4096,
  nameChars: 16,
  versionChars: 16,
  platformChars: 16,
  lanCandidates: 4,
  candidates: 6,
  friends: 200,
  pendingOut: 50,
  pendingIn: 50,
  requestDays: 14,
  registrationsPerIpPerDay: 20,
  socketsPerIp: 16,
  graceMs: 30000,
  inviteMs: 60000,
  handleMs: 120000,
  openRoomMs: 3600 * 1000,
  roomMaxMs: 4 * 3600 * 1000,
  ruleFields: 16,
  searchResults: 20,
  invitesPerRoom: 3,
};

export const BUCKETS = {
  any: { burst: 60, perSecond: 20 },
  social: { burst: 10, perSecond: 1 },
};

// Binary relay frames: magic "EK", version, type, token (u32), sequence (u32), destination seat, source seat,
// payload length (u16), payload.
export const RELAY = {
  version: 1,
  data: 1,
  headerBytes: 16,
  maxPayload: 1472,
  broadcast: 0xff,
  connection: { packets: { burst: 800, perSecond: 400 }, bytes: { burst: 256 * 1024, perSecond: 128 * 1024 } },
  overrunsToClose: 256,
  invalid: { burst: 200, perSecond: 5 },
  bufferedBytes: 256 * 1024,
  backpressureDropsToClose: 1024,
  loggedForwards: 3,
};

// A sender is charged once per frame, the room once per delivery: a broadcast costs the room what its fan-out costs the
// object. Each seat brings one connection's budget.
function roomBudget(capacity) {
  const { packets, bytes } = RELAY.connection;
  const scaled = ({ burst, perSecond }) => ({ burst: burst * capacity, perSecond: perSecond * capacity });
  return { packets: scaled(packets), bytes: scaled(bytes) };
}

export const RELAY_ROOM = Object.fromEntries([2, 3, 4].map((capacity) => [capacity, roomBudget(capacity)]));

export const CAPABILITIES = {
  protocol_version: PROTOCOL_VERSION,
  worker_build: WORKER_BUILD,
  public_endpoints: true,
  relay_supported: true,
  relay_max_payload: RELAY.maxPayload,
  min_players: MIN_PLAYERS,
  max_players: MAX_PLAYERS,
  four_player_rooms: true,
  seat_routing: true,
  race_telemetry: true,
};

export const healthReport = () => ({ service: SERVICE, game: GAME_ID, protocol: PROTOCOL_VERSION, status: 'ok',
  worker_build: WORKER_BUILD, relay_supported: true, relay_max_payload: RELAY.maxPayload });

// The header of one relay frame, or the reason it is not one. The payload is never looked at.
export function parseRelayFrame(bytes) {
  if (!(bytes instanceof Uint8Array)) return { error: 'not_binary' };
  if (bytes.length < 4) return { error: 'truncated' };
  if (bytes[0] !== 0x45 || bytes[1] !== 0x4b) return { error: 'bad_magic' };
  if (bytes[2] !== RELAY.version) return { error: 'bad_version' };
  if (bytes[3] !== RELAY.data) return { error: 'bad_type' };
  if (bytes.length < RELAY.headerBytes) return { error: 'truncated' };
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(14);
  if (length > RELAY.maxPayload || bytes.length > RELAY.headerBytes + RELAY.maxPayload) return { error: 'oversized' };
  if (length === 0 || bytes.length !== RELAY.headerBytes + length) return { error: 'bad_length' };
  return { type: bytes[3], token: view.getUint32(4), seq: view.getUint32(8), dest: bytes[12], length };
}

export function encodeRelayFrame(token, seq, dest, payload, src = 0) {
  const bytes = new Uint8Array(RELAY.headerBytes + payload.length);
  const view = new DataView(bytes.buffer);
  bytes.set([0x45, 0x4b, RELAY.version, RELAY.data]);
  view.setUint32(4, token >>> 0);
  view.setUint32(8, seq >>> 0);
  bytes[12] = dest;
  bytes[13] = src;
  view.setUint16(14, payload.length);
  bytes.set(payload, RELAY.headerBytes);
  return bytes;
}

const PLAYER_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

export class ProtocolError extends Error {
  constructor(code, detail = '') {
    super(detail || code);
    this.code = code;
  }
}

export const fail = (code, detail) => {
  throw new ProtocolError(code, detail);
};

export const tokenHash = (token) => sha256Hex(`eck-token:${token}`);

// The public id is also the friend code. The client derives it the same way.
export function playerIdFor(token) {
  const digest = sha256Hex(`eck-player-id:${token}`);
  let id = '';
  for (let i = 0; i < 8; i++) {
    id += PLAYER_ALPHABET[parseInt(digest.slice(i * 2, i * 2 + 2), 16) % PLAYER_ALPHABET.length];
    if (i === 3) id += '-';
  }
  return id;
}

export function normalizeFriendCode(value) {
  if (typeof value !== 'string') fail('bad_request', 'code expected');
  const compact = value.toUpperCase().replace(/[\s-]/g, '');
  if (compact.length !== 8 || [...compact].some((c) => !PLAYER_ALPHABET.includes(c))) fail('bad_request', 'malformed code');
  return `${compact.slice(0, 4)}-${compact.slice(4)}`;
}

export function cleanName(value) {
  if (typeof value !== 'string') fail('bad_request', 'name expected');
  const name = value.replace(/[^A-Za-z0-9_-]/g, '').slice(0, LIMITS.nameChars);
  return name || 'Player';
}

export function cleanLabel(value, maxChars) {
  if (value === undefined) return '';
  if (typeof value !== 'string') fail('bad_request', 'text expected');
  return value.replace(/[^A-Za-z0-9 ._+-]/g, '').slice(0, maxChars);
}

export function parseHello(message, images = SUPPORTED_IMAGES) {
  if (message.proto !== PROTOCOL_VERSION) fail('proto', 'protocol version not supported');
  if (message.game !== GAME_ID) fail('bad_game', 'unknown game');
  if (typeof message.image !== 'string' || !images.includes(message.image)) fail('bad_image', 'unsupported game image');
  if (!Number.isInteger(message.build) || message.build < 0 || message.build > 0xffff) {
    fail('bad_request', 'build expected');
  }
  if (typeof message.token !== 'string' || !/^[0-9a-f]{64}$/.test(message.token)) {
    fail('bad_request', 'token expected');
  }
  if (message.player !== playerIdFor(message.token)) fail('unauthorized', 'player id does not match');
  return {
    player: message.player,
    hash: tokenHash(message.token),
    name: cleanName(message.name ?? 'Player'),
    image: message.image,
    build: message.build,
    version: cleanLabel(message.version, LIMITS.versionChars),
    platform: cleanLabel(message.platform, LIMITS.platformChars),
    presence: message.presence !== false,
    relay: message.relay === true,
  };
}

export function ipv4Kind(ip) {
  const m = typeof ip === 'string' ? /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(ip) : null;
  if (!m) return null;
  const [a, b, c, d] = m.slice(1).map(Number);
  if ([a, b, c, d].some((n) => n > 255)) return null;
  if (a === 127) return 'loopback';
  if (a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254)) {
    return 'private';
  }
  return 'public';
}

const CANDIDATE_KINDS = ['host', 'srflx', 'upnp'];
const isPort = (v, lo) => Number.isInteger(v) && v >= lo && v <= 65535;

export function parseEndpoint(message) {
  if (!isPort(message.port, 1024)) fail('bad_request', 'port expected');
  const lan = message.lan ?? [];
  if (!Array.isArray(lan) || lan.length > LIMITS.lanCandidates) fail('bad_request', 'lan expected');
  for (const ip of lan) {
    const kind = ipv4Kind(ip);
    if (kind !== 'loopback' && kind !== 'private') fail('bad_request', 'lan address expected');
  }
  // The sender's gameplay socket as STUN saw it. This service never sees UDP, so it passes the claim on as it is.
  let publicIp = '';
  let publicPort = 0;
  if (message.public_ip !== undefined || message.public_port !== undefined) {
    if (typeof message.public_ip !== 'string' || ipv4Kind(message.public_ip) !== 'public') {
      fail('bad_request', 'public_ip expected');
    }
    if (!isPort(message.public_port, 1)) fail('bad_request', 'public_port expected');
    publicIp = message.public_ip;
    publicPort = message.public_port;
  }
  const raw = message.candidates ?? [];
  if (!Array.isArray(raw) || raw.length > LIMITS.candidates) fail('bad_request', 'candidates expected');
  const candidates = raw.map((c) => {
    if (c === null || typeof c !== 'object' || !ipv4Kind(c.ip) || !isPort(c.port, 1) || !CANDIDATE_KINDS.includes(c.kind)) {
      fail('bad_request', 'candidate expected');
    }
    return { ip: c.ip, port: c.port, kind: c.kind };
  });
  if (message.relay !== undefined && typeof message.relay !== 'boolean') fail('bad_request', 'relay expected');
  return { port: message.port, lan: [...lan], publicIp, publicPort, candidates, relay: message.relay === true };
}

// A public address in an endpoint can only be the sender's own. Over IPv4 the connection proves which one that is, so a
// claim of any other is dropped and peers are never pointed at a third party.
export function ownEndpoint(endpoint, ip) {
  if (ipv4Kind(ip) !== 'public') return endpoint;
  const own = (address) => ipv4Kind(address) !== 'public' || address === ip;
  const claimed = !endpoint.publicPort || own(endpoint.publicIp);
  return { ...endpoint, publicIp: claimed ? endpoint.publicIp : '', publicPort: claimed ? endpoint.publicPort : 0,
    candidates: endpoint.candidates.filter((c) => own(c.ip)) };
}

const RULE_KEY = /^[a-z_]{1,16}$/;

export function parseRules(value) {
  const rules = value ?? {};
  if (rules === null || typeof rules !== 'object' || Array.isArray(rules) || Object.keys(rules).length > LIMITS.ruleFields) {
    fail('bad_request', 'rules expected');
  }
  const clean = {};
  for (const [key, v] of Object.entries(rules)) {
    if (!RULE_KEY.test(key) || !Number.isInteger(v) || v < 0 || v > 0xffff) fail('bad_request', 'rule expected');
    clean[key] = v;
  }
  return clean;
}

export function parseCapacity(value) {
  const capacity = value ?? 2;
  if (!Number.isInteger(capacity) || capacity < MIN_PLAYERS || capacity > MAX_PLAYERS) fail('bad_request', 'capacity expected');
  return capacity;
}

// Per-address limits group IPv6 clients by their /64, whatever the spelling.
export function ipGroup(ip) {
  if (typeof ip !== 'string' || !ip.includes(':')) return String(ip);
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i.exec(ip);
  if (mapped) return mapped[1];
  const [head, tail] = ip.split('::');
  const before = head ? head.split(':') : [];
  const after = tail ? tail.split(':') : [];
  const gap = tail === undefined ? [] : Array(Math.max(0, 8 - before.length - after.length)).fill('0');
  return [...before, ...gap, ...after].slice(0, 4).map((g) => g.toLowerCase().replace(/^0+(?=.)/, '')).join(':');
}
export const isLocalAddress = (ip) => ip === '::1' || ipv4Kind(ip) === 'loopback';

export function compatible(a, b) {
  return Boolean(a && b) && a.image === b.image && a.build === b.build;
}

const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
const RACE_INDEX_MAX = 0xffff;
const RACE_PLACE_MAX = 16;

function parseCharacters(value) {
  if (value === undefined) return null;
  if (!Array.isArray(value) || value.length < 1 || value.length > MAX_PLAYERS || value.some((c) => !VALID_CHARACTERS.has(c))) {
    fail('bad_request', 'characters expected');
  }
  return [...value];
}

function parseTrack(value) {
  if (value === undefined) return null;
  if (!VALID_TRACKS.has(value)) fail('bad_request', 'track expected');
  return value;
}

function parsePlaces(value) {
  if (value === undefined) return null;
  if (!Array.isArray(value) || value.length < 1 || value.length > MAX_PLAYERS || value.some((p) => !isInt(p, 1, RACE_PLACE_MAX)) ||
      new Set(value).size !== value.length) {
    fail('bad_request', 'places expected');
  }
  return [...value];
}

// `characters` and `track` stay optional: the client leaves out what it could not prove.
export function parseRaceStart(message) {
  if (!isInt(message.race, 0, RACE_INDEX_MAX)) fail('bad_request', 'race expected');
  if (!VALID_COMPETITIONS.has(message.competition)) fail('bad_request', 'competition expected');
  if (message.players !== undefined && !isInt(message.players, 1, MAX_PLAYERS)) fail('bad_request', 'players expected');
  return { race: message.race, competition: message.competition, players: message.players ?? null,
    characters: parseCharacters(message.characters), track: parseTrack(message.track) };
}

// `places` is each human's RaceFinishPosition, in the order of the race.start characters.
export function parseRaceComplete(message) {
  if (!isInt(message.race, 0, RACE_INDEX_MAX)) fail('bad_request', 'race expected');
  if (!isInt(message.duration_ms, RACE_DURATION.minMs, RACE_DURATION.maxMs)) fail('bad_request', 'duration_ms expected');
  return { race: message.race, durationMs: message.duration_ms, places: parsePlaces(message.places) };
}
