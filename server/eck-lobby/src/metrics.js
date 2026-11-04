// Aggregate service metrics for the private dashboard (README.md, Metrics): daily counters, daily counts per dimension,
// 15-minute usage samples and a bounded list of recently closed rooms. No row names a player, an address or a room, and
// nothing here keeps per-session state: live numbers come from the hub's sockets and tables.
export const METRICS_TIME_ZONE = 'America/Sao_Paulo';
export const SAMPLE_MS = 15 * 60 * 1000;
export const SAMPLE_DAYS = 90;
export const HISTORY_MAX_DAYS = 365;
export const DIMENSIONS = ['platform', 'version', 'country', 'reject', 'transport', 'room_capacity', 'room_occupancy',
  'room_lifetime', 'abuse', 'competition', 'player_count', 'race_context', 'race_transport', 'race_duration_bucket', 'races_per_room'];
export const LIFETIMES = [['under_1m', 60e3], ['1_5m', 5 * 60e3], ['5_15m', 15 * 60e3], ['15_60m', 60 * 60e3], ['over_1h', Infinity]];
export const RACE_DURATION_BUCKETS = [['under_2m', 2 * 60e3], ['2_4m', 4 * 60e3], ['4_6m', 6 * 60e3], ['6_10m', 10 * 60e3],
  ['over_10m', Infinity]];
export const DIAGNOSTICS = new Set(['ws_upgrade_accepted', 'ws_upgrade_limited', 'ws_upgrade_ip_cap', 'hello_accepted',
  'hello_reconnect_deduped', 'hello_limited', 'room_host_accepted', 'room_host_limited', 'room_join_limited', 'invite_limited',
  'room_resume_limited', 'relay_abuse_closed', 'relay_backpressure_drop', 'race_limited']);
export const PLATFORMS = ['Windows', 'Linux', 'Steam Deck'];

const DAY_MS = 24 * 3600 * 1000;
// Bounds what a client can add to the table: further new versions in a day count as Other.
const VALUES_PER_DIMENSION_PER_DAY = 64;
const CLEANUP_ROWS = 500;
export const RECENT_ROOMS_MAX = 50;
export const RECENT_ROOMS_DAYS = 30;
export const RECENT_ROOMS_DEFAULT = 20;

const COUNTERS = new Set(['sessions', 'rooms_created', 'rooms_started', 'joins', 'invites', 'incompatible_rejects',
  'direct_selected', 'relay_selected', 'races_started', 'races_completed', 'races_abandoned', 'rooms_with_race',
  'rooms_rematched']);
const ROOM_COLUMNS = ['rooms_closed', 'rooms_with_guest', 'rooms_direct', 'rooms_relay', 'rooms_pending'];
const RACE_SUM_COLUMNS = new Set(['race_duration_ms', 'races_in_rooms']);
const SUMMED = [...COUNTERS, ...ROOM_COLUMNS, ...RACE_SUM_COLUMNS];
const PEAKS = ['peak_online', 'peak_in_rooms', 'peak_rooms'];
const DAY_COLUMNS = [...SUMMED, ...PEAKS];
const PLAYER_COUNT_TRANSPORTS = ['direct', 'relay', 'mixed'];
const CROSS_KINDS = ['competition_players', 'track_players', 'track_competition', 'character_competition', 'character_track'];

export const METRICS_SCHEMA = [
  `CREATE TABLE IF NOT EXISTS metrics_daily (day TEXT PRIMARY KEY,
     ${DAY_COLUMNS.map((c) => `${c} INTEGER NOT NULL DEFAULT 0`).join(', ')})`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_dimensions (day TEXT NOT NULL, dimension TEXT NOT NULL, value TEXT NOT NULL,
     count INTEGER NOT NULL, PRIMARY KEY (day, dimension, value))`,
  `CREATE TABLE IF NOT EXISTS metrics_samples (bucket INTEGER PRIMARY KEY, online INTEGER NOT NULL,
     in_rooms INTEGER NOT NULL, rooms INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS recent_rooms (seq INTEGER PRIMARY KEY AUTOINCREMENT, ended INTEGER NOT NULL,
     duration_ms INTEGER NOT NULL, capacity INTEGER NOT NULL, peak INTEGER NOT NULL, transport TEXT NOT NULL,
     platforms TEXT NOT NULL, state TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_player_counts (day TEXT NOT NULL, player_count INTEGER NOT NULL,
     races INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0, abandoned INTEGER NOT NULL DEFAULT 0,
     duration_ms INTEGER NOT NULL DEFAULT 0, ${PLAYER_COUNT_TRANSPORTS.map((c) => `${c} INTEGER NOT NULL DEFAULT 0`).join(', ')},
     PRIMARY KEY (day, player_count))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_race_outcomes (day TEXT NOT NULL, dimension TEXT NOT NULL, value TEXT NOT NULL,
     races INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0, abandoned INTEGER NOT NULL DEFAULT 0,
     duration_ms INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (day, dimension, value))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_race_cross (day TEXT NOT NULL, kind TEXT NOT NULL, a TEXT NOT NULL, b TEXT NOT NULL,
     count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (day, kind, a, b))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_characters (day TEXT NOT NULL, character INTEGER NOT NULL,
     picks INTEGER NOT NULL DEFAULT 0, races INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0,
     PRIMARY KEY (day, character))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_finish_positions (day TEXT NOT NULL, character INTEGER NOT NULL,
     player_count INTEGER NOT NULL, position INTEGER NOT NULL, count INTEGER NOT NULL DEFAULT 0,
     PRIMARY KEY (day, character, player_count, position))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_character_pairs (day TEXT NOT NULL, a INTEGER NOT NULL, b INTEGER NOT NULL,
     races INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (day, a, b))`,
  `CREATE TABLE IF NOT EXISTS metrics_daily_compositions (day TEXT NOT NULL, player_count INTEGER NOT NULL, composition TEXT NOT NULL,
     races INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (day, player_count, composition))`,
];

const dayFormat = new Intl.DateTimeFormat('en-CA', { timeZone: METRICS_TIME_ZONE, year: 'numeric', month: '2-digit',
  day: '2-digit' });

export function metricsDay(ms) {
  const parts = Object.fromEntries(dayFormat.formatToParts(ms).map((p) => [p.type, p.value]));
  return `${parts.year}-${parts.month}-${parts.day}`;
}

function daysBefore(day, n) {
  const [y, m, d] = day.split('-').map(Number);
  return new Date(Date.UTC(y, m - 1, d - n)).toISOString().slice(0, 10);
}

export const sampleBucket = (ms) => ms - (ms % SAMPLE_MS);

export const platformCategory = (text) => (PLATFORMS.includes(text) ? text : 'Other');

export function versionCategory(text) {
  if (!text) return 'Unknown';
  return /^\d{1,4}(\.\d{1,4}){1,3}$/.test(text) ? text : 'Other';
}

// Cloudflare's country code, or Unknown (XX is Cloudflare's own unknown, T1 is Tor).
export function countryCode(text) {
  return typeof text === 'string' && /^[A-Z]{2}$/.test(text) && text !== 'XX' ? text : 'Unknown';
}

// Each human's rank among the humans of the race, 1 for the best place.
function humanRanks(places) {
  const order = places.map((place, human) => ({ place, human })).sort((a, b) => a.place - b.place);
  const ranks = [];
  order.forEach(({ human }, i) => { ranks[human] = i + 1; });
  return ranks;
}

// One row per distinct unordered pair in the race, however many seats share a character; a character pairs with itself
// only when two seats picked it.
function characterPairs(characters) {
  const ids = [...characters].sort((a, b) => a - b);
  const pairs = new Map();
  for (let i = 0; i < ids.length; i++) {
    for (let j = i + 1; j < ids.length; j++) pairs.set(`${ids[i]}+${ids[j]}`, [ids[i], ids[j]]);
  }
  return [...pairs.values()];
}

const tally = (values) => {
  const counts = new Map();
  for (const value of values) counts.set(value, (counts.get(value) ?? 0) + 1);
  return counts;
};

const sorted = (counts) => [...counts].map(([value, count]) => ({ value, count }))
  .sort((a, b) => b.count - a.count || a.value.localeCompare(b.value));

// CREATE TABLE IF NOT EXISTS leaves a table that already exists in production as it was.
function ensureColumns(sql, table, columns) {
  const have = new Set(sql.all(`PRAGMA table_info(${table})`).map((c) => c.name));
  for (const column of columns) {
    if (!have.has(column)) sql.run(`ALTER TABLE ${table} ADD COLUMN ${column} INTEGER NOT NULL DEFAULT 0`);
  }
}

export class Metrics {
  constructor(sql, now) {
    this.sql = sql;
    this.now = now;
    for (const statement of METRICS_SCHEMA) sql.run(statement);
    ensureColumns(sql, 'metrics_daily', DAY_COLUMNS);
    ensureColumns(sql, 'metrics_daily_player_counts', PLAYER_COUNT_TRANSPORTS);
    this.peaks = null;
    this.sample = null;
    this.pending = new Map();
  }

  // Abuse diagnostics are counted in memory and written together by flushDiagnostics; true when this is the first
  // pending one.
  diagnose(name, amount = 1) {
    if (!DIAGNOSTICS.has(name)) throw new Error(`unknown diagnostic ${name}`);
    const first = this.pending.size === 0;
    this.pending.set(name, (this.pending.get(name) ?? 0) + amount);
    return first;
  }

  flushDiagnostics() {
    if (!this.pending.size) return;
    const day = metricsDay(this.now());
    const counts = [...this.pending];
    this.pending.clear();
    for (const [name, count] of counts) {
      this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, 'abuse', ?, ?)
        ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + excluded.count`, day, name, count);
    }
  }

  count(column, day = metricsDay(this.now())) {
    if (!COUNTERS.has(column)) throw new Error(`unknown counter ${column}`);
    this.sql.run(`INSERT INTO metrics_daily (day, ${column}) VALUES (?, 1)
      ON CONFLICT(day) DO UPDATE SET ${column} = ${column} + 1`, day);
  }

  addDimension(day, dimension, value) {
    const known = this.sql.get('SELECT 1 AS x FROM metrics_daily_dimensions WHERE day = ? AND dimension = ? AND value = ?',
      day, dimension, value);
    if (!known) {
      const values = this.sql.get('SELECT COUNT(*) AS n FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?',
        day, dimension).n;
      if (values >= VALUES_PER_DIMENSION_PER_DAY) value = 'Other';
    }
    this.sql.run(`INSERT INTO metrics_daily_dimensions (day, dimension, value, count) VALUES (?, ?, ?, 1)
      ON CONFLICT(day, dimension, value) DO UPDATE SET count = count + 1`, day, dimension, value);
  }

  // One connection that became an online session; `client` is its platform, version and country as reported.
  session(client) {
    const day = metricsDay(this.now());
    this.count('sessions', day);
    this.addDimension(day, 'platform', platformCategory(client.platform));
    this.addDimension(day, 'version', versionCategory(client.version));
    this.addDimension(day, 'country', countryCode(client.country));
  }

  roomCreated(capacity) {
    const day = metricsDay(this.now());
    this.count('rooms_created', day);
    this.addDimension(day, 'room_capacity', String(capacity));
  }

  // A seat's first path choice; `choice` is direct or relay.
  transport(choice) {
    const day = metricsDay(this.now());
    this.count(`${choice}_selected`, day);
    this.addDimension(day, 'transport', choice);
  }

  // `transport`: direct, relay, mixed or pending. Occupancy is only meaningful once a second player came.
  roomClosed({ capacity, peak, lifetimeMs, transport }) {
    const day = metricsDay(this.now());
    const guest = peak > 1;
    const flag = (kinds) => (guest && kinds.includes(transport) ? 1 : 0);
    this.sql.run(`INSERT INTO metrics_daily (day, rooms_closed, rooms_with_guest, rooms_direct, rooms_relay, rooms_pending)
      VALUES (?, 1, ?, ?, ?, ?) ON CONFLICT(day) DO UPDATE SET rooms_closed = rooms_closed + 1,
      rooms_with_guest = rooms_with_guest + excluded.rooms_with_guest, rooms_direct = rooms_direct + excluded.rooms_direct,
      rooms_relay = rooms_relay + excluded.rooms_relay, rooms_pending = rooms_pending + excluded.rooms_pending`,
    day, guest ? 1 : 0, flag(['direct']), flag(['relay', 'mixed']), flag(['pending']));
    if (guest) this.addDimension(day, 'room_occupancy', `${Math.min(peak, capacity)}/${capacity}`);
    this.addDimension(day, 'room_lifetime', LIFETIMES.find(([, limit]) => lifetimeMs < limit)[0]);
  }

  // Adds `amounts` to one row per key; one statement however many rows.
  addRows(table, keyColumns, amountColumns, rows) {
    if (!rows.length) return;
    const columns = ['day', ...keyColumns, ...amountColumns];
    const row = `(${columns.map(() => '?').join(', ')})`;
    const updates = amountColumns.map((c) => `${c} = ${c} + excluded.${c}`).join(', ');
    const day = metricsDay(this.now());
    this.sql.run(`INSERT INTO ${table} (${columns.join(', ')}) VALUES ${rows.map(() => row).join(', ')}
      ON CONFLICT(${['day', ...keyColumns].join(', ')}) DO UPDATE SET ${updates}`, ...rows.flatMap((r) => [day, ...r]));
  }

  addOutcome(race, amounts) {
    const columns = Object.keys(amounts);
    const values = Object.values(amounts);
    this.addRows('metrics_daily_player_counts', ['player_count'], columns, [[race.playerCount, ...values]]);
    const groups = [['competition', race.competition]];
    if (race.track !== null && race.track !== undefined) groups.push(['track', String(race.track)]);
    const shared = columns.filter((c) => !PLAYER_COUNT_TRANSPORTS.includes(c));
    this.addRows('metrics_daily_race_outcomes', ['dimension', 'value'], shared,
      groups.map((g) => [...g, ...shared.map((c) => amounts[c])]));
  }

  // `race`: { competition, track, playerCount, characters, online }; track and characters are null when the client could
  // not prove them. Competition and player count are tagged here only, so a race that later ends is not counted twice.
  raceStarted(race) {
    const day = metricsDay(this.now());
    const { competition, track, playerCount, characters } = race;
    this.count('races_started', day);
    this.addDimension(day, 'competition', competition);
    this.addDimension(day, 'player_count', String(playerCount));
    this.addDimension(day, 'race_context', race.online ? 'online' : 'local');
    this.addOutcome(race, { races: 1 });
    const hasTrack = track !== null && track !== undefined;
    const cross = [['competition_players', competition, String(playerCount), 1]];
    if (hasTrack) {
      cross.push(['track_players', String(track), String(playerCount), 1], ['track_competition', String(track), competition, 1]);
    }
    if (characters) {
      const picks = tally(characters);
      for (const [character, count] of picks) {
        cross.push(['character_competition', String(character), competition, count]);
        if (hasTrack) cross.push(['character_track', String(character), String(track), count]);
      }
      this.addRows('metrics_daily_characters', ['character'], ['picks', 'races'], [...picks].map(([c, count]) => [c, count, 1]));
      if (playerCount >= 2) {
        this.addRows('metrics_daily_character_pairs', ['a', 'b'], ['races'], characterPairs(characters).map(([a, b]) => [a, b, 1]));
        const composition = [...characters].sort((a, b) => a - b).join('+');
        this.addRows('metrics_daily_compositions', ['player_count', 'composition'], ['races'], [[playerCount, composition, 1]]);
      }
    }
    this.addRows('metrics_daily_race_cross', ['kind', 'a', 'b'], ['count'], cross);
  }

  // `places`, when given, has one distinct RaceFinishPosition per character. A lone human has nobody to rank against.
  raceCompleted(race) {
    const day = metricsDay(this.now());
    const { playerCount, characters, durationMs, transport, places } = race;
    this.count('races_completed', day);
    if (transport) this.addDimension(day, 'race_transport', transport);
    this.addDimension(day, 'race_duration_bucket', RACE_DURATION_BUCKETS.find(([, limit]) => durationMs < limit)[0]);
    this.addAmount('race_duration_ms', durationMs, day);
    const outcome = { completed: 1, duration_ms: durationMs };
    if (PLAYER_COUNT_TRANSPORTS.includes(transport)) outcome[transport] = 1;
    this.addOutcome(race, outcome);
    if (!characters) return;
    this.addRows('metrics_daily_characters', ['character'], ['completed'], [...tally(characters)]);
    if (!places || playerCount < 2) return;
    const ranks = humanRanks(places);
    const finishes = tally(characters.map((character, i) => `${character}:${ranks[i]}`));
    this.addRows('metrics_daily_finish_positions', ['character', 'player_count', 'position'], ['count'],
      [...finishes].map(([key, count]) => {
        const [character, position] = key.split(':').map(Number);
        return [character, playerCount, position, count];
      }));
  }

  raceAbandoned(race) {
    this.count('races_abandoned');
    this.addOutcome(race, { abandoned: 1 });
  }

  playerCounts(from, to) {
    return this.sql.all(`SELECT player_count, SUM(races) AS races, SUM(completed) AS completed, SUM(abandoned) AS abandoned,
      SUM(duration_ms) AS duration_ms, SUM(direct) AS direct, SUM(relay) AS relay, SUM(mixed) AS mixed
      FROM metrics_daily_player_counts WHERE day >= ? AND day <= ? GROUP BY player_count ORDER BY player_count`, from, to);
  }

  raceOutcomes(dimension, from, to) {
    return this.sql.all(`SELECT value, SUM(races) AS races, SUM(completed) AS completed, SUM(abandoned) AS abandoned,
      SUM(duration_ms) AS duration_ms FROM metrics_daily_race_outcomes WHERE dimension = ? AND day >= ? AND day <= ?
      GROUP BY value ORDER BY races DESC, value`, dimension, from, to);
  }

  raceCross(from, to) {
    const cross = Object.fromEntries(CROSS_KINDS.map((k) => [k, []]));
    for (const r of this.sql.all(`SELECT kind, a, b, SUM(count) AS count FROM metrics_daily_race_cross WHERE day >= ? AND day <= ?
      GROUP BY kind, a, b ORDER BY count DESC, a, b`, from, to)) cross[r.kind]?.push({ a: r.a, b: r.b, count: r.count });
    return cross;
  }

  characterStats(from, to, previousFrom, previousTo) {
    const picksBefore = new Map(this.sql.all(`SELECT character, SUM(picks) AS picks FROM metrics_daily_characters
      WHERE day >= ? AND day <= ? GROUP BY character`, previousFrom, previousTo).map((r) => [r.character, r.picks]));
    const finishes = this.sql.all(`SELECT character, player_count, position, SUM(count) AS count FROM metrics_daily_finish_positions
      WHERE day >= ? AND day <= ? GROUP BY character, player_count, position`, from, to);
    return this.sql.all(`SELECT character, SUM(picks) AS picks, SUM(races) AS races, SUM(completed) AS completed
      FROM metrics_daily_characters WHERE day >= ? AND day <= ? GROUP BY character ORDER BY picks DESC, character`, from, to)
      .map((row) => {
        const mine = finishes.filter((f) => f.character === row.character);
        const count = (test) => mine.filter(test).reduce((sum, f) => sum + f.count, 0);
        const ranked = count(() => true);
        const positions = [1, 2, 3, 4].map((position) => count((f) => f.position === position));
        return { ...row, previous_picks: picksBefore.get(row.character) ?? 0, ranked, positions, wins: positions[0],
          podiums: count((f) => f.position <= Math.min(3, f.player_count - 1)),
          last_places: count((f) => f.position === f.player_count),
          average_finish: ranked ? Math.round(100 * mine.reduce((sum, f) => sum + f.position * f.count, 0) / ranked) / 100 : null };
      });
  }

  characterCombinations(from, to) {
    const compositions = Object.fromEntries([2, 3, 4].map((n) => [n, []]));
    for (const r of this.sql.all(`SELECT player_count, composition, SUM(races) AS races FROM metrics_daily_compositions
      WHERE day >= ? AND day <= ? GROUP BY player_count, composition ORDER BY races DESC, composition`, from, to)) {
      compositions[r.player_count]?.push({ characters: r.composition.split('+').map(Number), races: r.races });
    }
    const pairs = this.sql.all(`SELECT a, b, SUM(races) AS races FROM metrics_daily_character_pairs WHERE day >= ? AND day <= ?
      GROUP BY a, b ORDER BY races DESC, a, b`, from, to);
    const multiplayer = Object.values(compositions).flat().reduce((sum, c) => sum + c.races, 0);
    return { pairs, compositions, multiplayer_races: multiplayer };
  }

  roomRaceCount(races) {
    if (races <= 0) return;
    const day = metricsDay(this.now());
    this.count('rooms_with_race', day);
    if (races >= 2) this.count('rooms_rematched', day);
    this.addAmount('races_in_rooms', races, day);
    this.addDimension(day, 'races_per_room', races >= 4 ? '4+' : String(races));
  }

  addAmount(column, amount, day = metricsDay(this.now())) {
    if (!RACE_SUM_COLUMNS.has(column)) throw new Error(`unknown sum column ${column}`);
    this.sql.run(`INSERT INTO metrics_daily (day, ${column}) VALUES (?, ?)
      ON CONFLICT(day) DO UPDATE SET ${column} = ${column} + excluded.${column}`, day, amount);
  }

  recordRoom(room) {
    const now = this.now();
    this.sql.run(`INSERT INTO recent_rooms (ended, duration_ms, capacity, peak, transport, platforms, state)
      VALUES (?, ?, ?, ?, ?, ?, ?)`, now, room.duration_ms, room.capacity, room.peak, room.transport, room.platforms, room.state);
    this.trimRecent(now);
  }

  trimRecent(now) {
    this.sql.run('DELETE FROM recent_rooms WHERE ended < ?', now - RECENT_ROOMS_DAYS * DAY_MS);
    this.sql.run(`DELETE FROM recent_rooms WHERE seq NOT IN
      (SELECT seq FROM recent_rooms ORDER BY seq DESC LIMIT ?)`, RECENT_ROOMS_MAX);
  }

  reject(reason) {
    const day = metricsDay(this.now());
    this.count('incompatible_rejects', day);
    this.addDimension(day, 'reject', reason);
  }

  // Raises today's peaks and the current sample to `live` ({ online, in_rooms, rooms }); writes only what grew.
  observe(live) {
    const now = this.now();
    const day = metricsDay(now);
    if (this.peaks?.day !== day) {
      const row = this.sql.get('SELECT peak_online, peak_in_rooms, peak_rooms FROM metrics_daily WHERE day = ?', day);
      this.peaks = row ? { day, online: row.peak_online, in_rooms: row.peak_in_rooms, rooms: row.peak_rooms }
        : { day, online: -1, in_rooms: -1, rooms: -1 };
    }
    if (grew(this.peaks, live)) {
      this.sql.run(`INSERT INTO metrics_daily (day, peak_online, peak_in_rooms, peak_rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(day) DO UPDATE SET peak_online = MAX(peak_online, excluded.peak_online),
        peak_in_rooms = MAX(peak_in_rooms, excluded.peak_in_rooms), peak_rooms = MAX(peak_rooms, excluded.peak_rooms)`,
      day, live.online, live.in_rooms, live.rooms);
      raise(this.peaks, live);
    }
    const bucket = sampleBucket(now);
    if (this.sample?.bucket !== bucket) {
      const row = this.sql.get('SELECT online, in_rooms, rooms FROM metrics_samples WHERE bucket = ?', bucket);
      this.sample = row ? { bucket, ...row } : { bucket, online: -1, in_rooms: -1, rooms: -1 };
      this.dropOldSamples(now);
      this.trimRecent(now);
    }
    if (grew(this.sample, live)) {
      this.sql.run(`INSERT INTO metrics_samples (bucket, online, in_rooms, rooms) VALUES (?, ?, ?, ?)
        ON CONFLICT(bucket) DO UPDATE SET online = MAX(online, excluded.online),
        in_rooms = MAX(in_rooms, excluded.in_rooms), rooms = MAX(rooms, excluded.rooms)`,
      bucket, live.online, live.in_rooms, live.rooms);
      raise(this.sample, live);
    }
  }

  dropOldSamples(now) {
    this.sql.run(`DELETE FROM metrics_samples WHERE bucket IN
      (SELECT bucket FROM metrics_samples WHERE bucket < ? ORDER BY bucket LIMIT ?)`, now - SAMPLE_DAYS * DAY_MS,
    CLEANUP_ROWS);
  }

  // `live`: the counts, Maps of platform, version and country to players online and of transport to seats, and the room list.
  summary(live) {
    const now = this.now();
    const day = metricsDay(now);
    const today = this.sql.get(`SELECT ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day = ?`, day) ?? emptyDay();
    return {
      generated: now,
      time_zone: METRICS_TIME_ZONE,
      day,
      live: { ...live, platform: sorted(live.platform), version: sorted(live.version), country: sorted(live.country),
        transport: sorted(live.transport) },
      today: { ...today, peak_online: Math.max(today.peak_online, live.online),
        peak_in_rooms: Math.max(today.peak_in_rooms, live.in_rooms), peak_rooms: Math.max(today.peak_rooms, live.rooms) },
    };
  }

  // The closed rooms of the range, newest first, with how many the range holds in all.
  recent(first, limit) {
    const since = Math.max(Date.parse(`${first}T00:00:00Z`) - DAY_MS, this.now() - RECENT_ROOMS_DAYS * DAY_MS);
    const rows = this.sql.all(`SELECT ended, duration_ms, capacity, peak, transport, platforms, state FROM recent_rooms
      WHERE ended >= ? ORDER BY seq DESC`, since).filter((r) => metricsDay(r.ended) >= first);
    return { limit, retention: { rooms: RECENT_ROOMS_MAX, days: RECENT_ROOMS_DAYS }, total: rows.length, rooms: rows.slice(0, limit) };
  }

  totals(from, to) {
    const select = [...SUMMED.map((c) => `COALESCE(SUM(${c}), 0) AS ${c}`), ...PEAKS.map((c) => `COALESCE(MAX(${c}), 0) AS ${c}`)];
    return this.sql.get(`SELECT ${select.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?`, from, to);
  }

  history(days, recentLimit = RECENT_ROOMS_DEFAULT) {
    if (!Number.isInteger(days) || days < 1 || days > HISTORY_MAX_DAYS) throw new RangeError('days out of range');
    if (!Number.isInteger(recentLimit) || recentLimit < 1 || recentLimit > RECENT_ROOMS_MAX) {
      throw new RangeError('recent out of range');
    }
    const now = this.now();
    const last = metricsDay(now);
    const first = daysBefore(last, days - 1);
    const rows = new Map(this.sql.all(`SELECT day, ${DAY_COLUMNS.join(', ')} FROM metrics_daily WHERE day >= ? AND day <= ?
      ORDER BY day`, first, last).map((r) => [r.day, r]));
    const daily = [];
    for (let i = days - 1; i >= 0; i--) {
      const day = daysBefore(last, i);
      daily.push(rows.get(day) ?? { day, ...emptyDay() });
    }
    const dimensions = Object.fromEntries(DIMENSIONS.map((d) => [d, []]));
    for (const r of this.sql.all(`SELECT dimension, value, SUM(count) AS count FROM metrics_daily_dimensions
      WHERE day >= ? AND day <= ? GROUP BY dimension, value ORDER BY count DESC, value`, first, last)) {
      dimensions[r.dimension]?.push({ value: r.value, count: r.count });
    }
    const since = Math.max(now - days * DAY_MS, now - SAMPLE_DAYS * DAY_MS);
    const samples = this.sql.all('SELECT bucket, online, in_rooms, rooms FROM metrics_samples WHERE bucket >= ? ORDER BY bucket',
      sampleBucket(since));
    const totals = this.totals(first, last);
    const previous = this.totals(daysBefore(first, days), daysBefore(first, 1));
    return { generated: now, time_zone: METRICS_TIME_ZONE, days, first, last, sample_ms: SAMPLE_MS, daily, dimensions, samples,
      totals, previous, comparison: compare(totals, previous), recent: this.recent(first, recentLimit),
      player_counts: this.playerCounts(first, last), competitions: this.raceOutcomes('competition', first, last),
      tracks: this.raceOutcomes('track', first, last), race_cross: this.raceCross(first, last),
      characters: this.characterStats(first, last, daysBefore(first, days), daysBefore(first, 1)),
      ...this.characterCombinations(first, last) };
  }
}

const emptyDay = () => Object.fromEntries(DAY_COLUMNS.map((c) => [c, 0]));

// Relative change in percent; null when there was nothing before to compare with.
export const percentChange = (current, previous) => (previous > 0 ? Math.round(1000 * (current - previous) / previous) / 10 : null);

export const COMPARED = ['sessions', 'rooms_created', 'rooms_started', 'rooms_with_guest', 'joins', 'invites', 'direct_selected',
  'relay_selected', 'peak_online', 'peak_in_rooms', 'peak_rooms', 'races_started', 'races_completed', 'races_abandoned',
  'rooms_with_race', 'rooms_rematched'];

export function compare(current, previous) {
  return Object.fromEntries(COMPARED.map((c) => [c, { current: current[c], previous: previous[c],
    change: percentChange(current[c], previous[c]) }]));
}

const grew = (seen, live) => live.online > seen.online || live.in_rooms > seen.in_rooms || live.rooms > seen.rooms;

function raise(seen, live) {
  seen.online = Math.max(seen.online, live.online);
  seen.in_rooms = Math.max(seen.in_rooms, live.in_rooms);
  seen.rooms = Math.max(seen.rooms, live.rooms);
}
