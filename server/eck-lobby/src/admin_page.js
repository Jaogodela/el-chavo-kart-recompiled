// The private dashboard (/admin): a static shell, its stylesheet and script, served same-origin under a strict CSP.
// The script keeps the token in memory (sessionStorage only when asked) and sends it as a Bearer header.
import { GAME_LABELS } from './game_data.js';

export const ADMIN_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex, nofollow">
<title>ECK Online Metrics</title>
<link rel="stylesheet" href="/admin/app.css">
</head>
<body>
<header class="top">
  <h1>El Chavo Kart · Online Metrics</h1>
  <p class="muted">Anonymous aggregate statistics of the online service. Days are America/Sao_Paulo.</p>
</header>
<main>
  <form id="login" class="panel login" autocomplete="off">
    <label for="token">Dashboard token</label>
    <input id="token" type="password" autocomplete="off" spellcheck="false" required>
    <label class="check"><input id="remember" type="checkbox"> Keep for this tab</label>
    <button type="submit">Open dashboard</button>
    <p id="login-error" class="error" role="alert"></p>
  </form>
  <section id="dash" hidden>
    <div class="toolbar">
      <label for="range">Range</label>
      <select id="range">
        <option value="7">7 days</option>
        <option value="30" selected>30 days</option>
        <option value="90">90 days</option>
        <option value="365">365 days</option>
      </select>
      <button id="refresh" type="button">Refresh</button>
      <span id="status" class="muted" role="status"></span>
      <button id="signout" type="button" class="quiet">Sign out</button>
    </div>
    <h2 class="section">Live now</h2>
    <p class="muted">Current state only: these change as players come and go and are not totals.</p>
    <div class="cards">
      <div class="card"><span class="label">Online now</span><span id="c-online" class="value">–</span></div>
      <div class="card"><span class="label">In rooms now</span><span id="c-in-rooms" class="value">–</span></div>
      <div class="card"><span class="label">Rooms advertised now</span><span id="c-advertised" class="value">–</span></div>
      <div class="card"><span class="label">Active rooms now</span><span id="c-active" class="value">–</span></div>
      <div class="card"><span class="label">Races in progress now</span><span id="c-racing" class="value">–</span></div>
      <div class="card"><span class="label">Direct now</span><span id="c-direct" class="value">–</span></div>
      <div class="card"><span class="label">Relay now</span><span id="c-relay" class="value">–</span></div>
      <div class="card"><span class="label">Pending now</span><span id="c-pending" class="value">–</span></div>
    </div>
    <div class="cards compact">
      <div class="card"><span class="label">2-player rooms</span><span id="c-rooms-2" class="value">–</span></div>
      <div class="card"><span class="label">3-player rooms</span><span id="c-rooms-3" class="value">–</span></div>
      <div class="card"><span class="label">4-player rooms</span><span id="c-rooms-4" class="value">–</span></div>
      <div class="card"><span class="label">Occupied seats</span><span id="c-occupied" class="value">–</span></div>
      <div class="card"><span class="label">Maximum seats</span><span id="c-max-seats" class="value">–</span></div>
      <div class="card"><span class="label">Average occupancy</span><span id="c-occupancy" class="value">–</span></div>
    </div>
    <h2 class="section">Today</h2>
    <p id="today-line" class="muted"></p>
    <div class="cards">
      <div class="card"><span class="label">Peak online today</span><span id="t-peak" class="value">–</span></div>
      <div class="card"><span class="label">Sessions today</span><span id="t-sessions" class="value">–</span></div>
      <div class="card"><span class="label">Rooms created today</span><span id="t-created" class="value">–</span></div>
      <div class="card"><span class="label">Rooms started today</span><span id="t-started" class="value">–</span></div>
      <div class="card"><span class="label">Rooms closed today</span><span id="t-closed" class="value">–</span></div>
      <div class="card"><span class="label">Joins today</span><span id="t-joins" class="value">–</span></div>
      <div class="card"><span class="label">Invites today</span><span id="t-invites" class="value">–</span></div>
      <div class="card"><span class="label">Direct selected today</span><span id="t-direct" class="value">–</span></div>
      <div class="card"><span class="label">Relay selected today</span><span id="t-relay" class="value">–</span></div>
      <div class="card"><span class="label">Races started today</span><span id="t-races-started" class="value">–</span></div>
      <div class="card"><span class="label">Races completed today</span><span id="t-races-completed" class="value">–</span></div>
      <div class="card"><span class="label">Races abandoned today</span><span id="t-races-abandoned" class="value">–</span></div>
    </div>
    <div class="panel">
      <h2 id="range-title">Selected range</h2>
      <div class="cards compact">
        <div class="card"><span class="label">Sessions</span><span id="r-sessions" class="value">–</span></div>
        <div class="card"><span class="label">Rooms created</span><span id="r-created" class="value">–</span></div>
        <div class="card"><span class="label">Rooms started</span><span id="r-started" class="value">–</span></div>
        <div class="card"><span class="label">Rooms closed</span><span id="r-closed" class="value">–</span></div>
        <div class="card"><span class="label">Rooms with a guest</span><span id="r-guest" class="value">–</span></div>
        <div class="card"><span class="label">Joins</span><span id="r-joins" class="value">–</span></div>
        <div class="card"><span class="label">Invites</span><span id="r-invites" class="value">–</span></div>
        <div class="card"><span class="label">Direct rooms</span><span id="r-rooms-direct" class="value">–</span></div>
        <div class="card"><span class="label">Relay rooms</span><span id="r-rooms-relay" class="value">–</span></div>
        <div class="card"><span class="label">Pending rooms</span><span id="r-rooms-pending" class="value">–</span></div>
        <div class="card"><span class="label">Races started</span><span id="r-races-started" class="value">–</span></div>
        <div class="card"><span class="label">Races completed</span><span id="r-races-completed" class="value">–</span></div>
        <div class="card"><span class="label">Races abandoned</span><span id="r-races-abandoned" class="value">–</span></div>
        <div class="card"><span class="label">Average race duration</span><span id="r-race-duration" class="value">–</span></div>
        <div class="card"><span class="label">Rooms with a race</span><span id="r-rooms-with-race" class="value">–</span></div>
        <div class="card"><span class="label">Rooms rematched</span><span id="r-rooms-rematched" class="value">–</span></div>
        <div class="card"><span class="label">Average races per room</span><span id="r-races-per-room" class="value">–</span></div>
      </div>
    </div>
    <div class="panel">
      <h2>Period comparison</h2>
      <p id="compare-note" class="muted"></p>
      <div class="scroll"><table id="compare"></table></div>
    </div>
    <div class="panel">
      <h2>Current rooms</h2>
      <div class="scroll"><table id="rooms"></table></div>
      <p id="rooms-empty" class="empty"></p>
    </div>
    <div class="panel">
      <h2 id="recent-title">Last rooms</h2>
      <div class="inline">
        <label for="recent-limit">Show</label>
        <select id="recent-limit">
          <option value="10">10</option>
          <option value="20" selected>20</option>
          <option value="50">50</option>
        </select>
      </div>
      <div class="scroll"><table id="recent"></table></div>
      <p id="recent-empty" class="empty"></p>
    </div>
    <div class="panel">
      <h2>Usage over time</h2>
      <p class="muted">Highest value in each 15-minute interval; kept for 90 days.</p>
      <div id="usage" class="chart"></div>
    </div>
    <div class="panel">
      <h2>Sessions by day</h2>
      <div id="sessions" class="chart"></div>
    </div>
    <div class="panel">
      <h2>Rooms by day</h2>
      <div id="rooms-by-day" class="chart"></div>
    </div>
    <div class="grid">
      <div class="panel"><h2>Room size</h2><div id="d-capacity"></div></div>
      <div class="panel"><h2>Room occupancy</h2><div id="d-occupancy"></div></div>
      <div class="panel"><h2>Room lifetime</h2><div id="d-lifetime"></div></div>
    </div>
    <h2 class="section">Races</h2>
    <p class="muted">Counted once per race, reported by the game; nothing here names a player or a room.</p>
    <div class="cards compact">
      <div class="card"><span class="label">Completion rate</span><span id="s-completion" class="value">–</span></div>
      <div class="card"><span class="label">Average players per race</span><span id="s-players" class="value">–</span></div>
      <div class="card"><span class="label">Most popular player count</span><span id="s-popular" class="value text">–</span></div>
      <div class="card"><span class="label">Most used character</span><span id="s-character" class="value text">–</span></div>
      <div class="card"><span class="label">Most played competition</span><span id="s-competition" class="value text">–</span></div>
      <div class="card"><span class="label">Most played track</span><span id="s-track" class="value text">–</span></div>
      <div class="card"><span class="label">Most common character pair</span><span id="s-pair" class="value text">–</span></div>
      <div class="card"><span class="label">Direct / Relay / Mixed</span><span id="s-transport" class="value text">–</span></div>
    </div>
    <div class="panel">
      <h2>Races by day</h2>
      <div id="races-by-day" class="chart"></div>
    </div>
    <div class="panel">
      <h2>Players per race</h2>
      <div class="scroll"><table id="d-player-count"></table></div>
      <p id="d-player-count-empty" class="empty"></p>
    </div>
    <div class="grid">
      <div class="panel"><h2>Race context</h2><div id="d-race-context"></div></div>
      <div class="panel"><h2>Race transport</h2><div id="d-race-transport"></div></div>
      <div class="panel"><h2>Race duration</h2><div id="d-race-duration"></div></div>
      <div class="panel"><h2>Races per room</h2><div id="d-races-per-room"></div></div>
    </div>
    <h2 class="section">Characters</h2>
    <div class="panel"><h2>Top Characters</h2><div id="d-top-characters"></div></div>
    <div class="panel">
      <h2>Character Picks</h2>
      <div class="scroll"><table id="character-picks"></table></div>
      <p id="character-picks-empty" class="empty"></p>
    </div>
    <div class="panel">
      <h2>Character Finish Results</h2>
      <p class="muted">Rank among the human players of a completed race, 2 players or more.</p>
      <div class="scroll"><table id="character-finishes"></table></div>
      <p id="character-finishes-empty" class="empty"></p>
    </div>
    <div class="grid">
      <div class="panel"><h2>Most Common Character Pairs</h2><div id="d-pairs"></div></div>
      <div class="panel"><h2>Most Common 2-Player Compositions</h2><div id="d-compositions-2"></div></div>
      <div class="panel"><h2>Most Common 3-Player Compositions</h2><div id="d-compositions-3"></div></div>
      <div class="panel"><h2>Most Common 4-Player Compositions</h2><div id="d-compositions-4"></div></div>
    </div>
    <div class="panel">
      <h2>Character Co-occurrence</h2>
      <p class="muted">Races where both characters appeared together.</p>
      <div class="scroll"><table id="pair-matrix"></table></div>
      <p id="pair-matrix-empty" class="empty"></p>
    </div>
    <h2 class="section">Competitions and tracks</h2>
    <div class="panel">
      <h2>Competitions</h2>
      <div class="scroll"><table id="competition-table"></table></div>
      <p id="competition-table-empty" class="empty"></p>
    </div>
    <div class="panel">
      <h2>Tracks</h2>
      <div class="scroll"><table id="track-table"></table></div>
      <p id="track-table-empty" class="empty"></p>
    </div>
    <div class="grid">
      <div class="panel"><h2>Characters by Competition</h2><div id="d-character-competition"></div></div>
      <div class="panel"><h2>Characters by Track</h2><div id="d-character-track"></div></div>
    </div>
    <div class="grid">
      <div class="panel"><h2>Platform</h2><div id="d-platform"></div></div>
      <div class="panel"><h2>Port version</h2><div id="d-version"></div></div>
      <div class="panel"><h2>Country</h2><div id="d-country"></div></div>
      <div class="panel"><h2>Transport</h2><div id="d-transport"></div></div>
    </div>
    <div class="panel">
      <h2>Daily totals</h2>
      <div class="scroll"><table id="daily"></table></div>
    </div>
    <details class="panel">
      <summary>Diagnostics: incompatibility rejects</summary>
      <div id="rejects"></div>
    </details>
    <details class="panel">
      <summary>Protection counters</summary>
      <div id="protection"></div>
    </details>
    <details class="panel">
      <summary>Diagnostics: abuse protection</summary>
      <div id="abuse"></div>
    </details>
    <div class="panel notes">
      <h2>How to read this</h2>
      <ul>
        <li>A session is one connection of a launcher or game that went online. The launcher and the game each count,
          and a reconnect within ten minutes of a counted session does not count again. Sessions are not players: there is
          no unique-player count.</li>
        <li>Platform, version and country by day count sessions started. "Now" counts players online, one per account,
          from the connections open at this moment.</li>
        <li>A room has 2, 3 or 4 seats. Advertised rooms are open to friends; active rooms have started and no longer take
          players. Occupied and maximum seats add up every room running now, and average occupancy is one over the other.</li>
        <li>Direct, Relay and Pending count seats, not rooms: each guest has its own path to the host, so one room can be mixed.
          A seat is Pending until the host chooses its path, and again if the relay closes.</li>
        <li>Room outcomes count rooms when they close. A Direct room used only direct paths, a Relay room used the relay for at
          least one seat, a Pending room had a guest but no path chosen. Occupancy is the most seats taken out of the room's
          size, for rooms that had a guest. Lifetime runs from creation to close and includes the 30-second seat grace.</li>
        <li>Period comparison sets the selected range (including today so far) against the same number of days before it. No
          percentage is shown when the earlier period has nothing to compare with.</li>
        <li>Last rooms keeps the latest 50 closed rooms that had a guest, for 30 days, with no names, codes, addresses, room ids
          or countries. Its final state is what the service can tell: Not started, Failed (started but never established),
          Ended (established), Disconnected (the host did not come back) or Timeout.</li>
        <li>Current rooms are read from the live service and shown with a short id only; none is stored for this page.</li>
        <li>Country comes from Cloudflare and is stored only as a daily count. Unknown: Cloudflare did not say.</li>
        <li>Races are reported by the game on its own connection to the service. A local race counts humans on one PC (1 to 4,
          split screen included) and has no transport. An online race is reported by the room's host only, so it counts once.</li>
        <li>A race is abandoned when it started and never completed: its room closed, its connection ended, or a new race began.
          Duration is the race itself, completed races only.</li>
        <li>Picks count seats; races count each race once. Finish results rank the humans of a completed race: a solo race has
          nobody to rank against, and Capture the Flag has no individual places, so neither adds to them. A win is rank 1, a podium is
          any rank that is not last (at most third), and last place is the worst rank of that race.</li>
        <li>A pair counts once per race for each two characters that raced together. Compositions keep every seat, so a character
          picked twice stays twice. Percentages use the races with known characters as their base.</li>
        <li>Character and track names are the game's own, by the ids in its script. Races the game could not identify still count
          in every other table.</li>
        <li>Nothing here stores names, Friend Codes, addresses, relay tokens or gameplay. Character combinations describe karts,
          never who played them.</li>
      </ul>
    </div>
  </section>
</main>
<div id="tip" class="tip" hidden></div>
<script src="/admin/app.js"></script>
</body>
</html>
`;

export const ADMIN_CSS = String.raw`:root {
  color-scheme: light;
  --surface-0: #f3f3f1;
  --surface-1: #fcfcfb;
  --border: #dcdbd6;
  --grid: #e7e6e2;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #6d6c67;
  --accent: #2a78d6;
  --series-1: #2a78d6;
  --series-2: #eb6834;
  --series-3: #1baf7a;
  --series-4: #8a5cd6;
  --error: #b42318;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) {
    color-scheme: dark;
    --surface-0: #111110;
    --surface-1: #1a1a19;
    --border: #34332f;
    --grid: #2a2a27;
    --text-primary: #f0efec;
    --text-secondary: #c3c2b7;
    --text-muted: #9a998f;
    --accent: #3987e5;
    --series-1: #3987e5;
    --series-2: #d95926;
    --series-3: #199e70;
    --series-4: #a07be0;
    --error: #f97066;
  }
}
:root[data-theme="dark"] {
  --series-4: #a07be0;
  color-scheme: dark;
  --surface-0: #111110;
  --surface-1: #1a1a19;
  --border: #34332f;
  --grid: #2a2a27;
  --text-primary: #f0efec;
  --text-secondary: #c3c2b7;
  --text-muted: #9a998f;
  --accent: #3987e5;
  --series-1: #3987e5;
  --series-2: #d95926;
  --series-3: #199e70;
  --error: #f97066;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  background: var(--surface-0);
  color: var(--text-primary);
  font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
.top, main { max-width: 1120px; margin: 0 auto; padding: 0 16px; }
.top { padding-top: 24px; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 15px; margin: 0 0 8px; }
.muted { color: var(--text-muted); margin: 0 0 12px; font-size: 13px; }
.panel {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px;
  margin: 0 0 16px;
  min-width: 0;
}
.login { max-width: 420px; display: grid; gap: 10px; }
.check { display: flex; gap: 8px; align-items: center; color: var(--text-secondary); }
input[type="password"], select {
  font: inherit;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--surface-0);
  color: var(--text-primary);
  width: 100%;
}
select { width: auto; }
button {
  font: inherit;
  padding: 8px 14px;
  border-radius: 6px;
  border: 1px solid var(--accent);
  background: var(--accent);
  color: #fff;
  cursor: pointer;
}
button.quiet { background: transparent; color: var(--text-secondary); border-color: var(--border); }
.error { color: var(--error); margin: 0; min-height: 1.2em; }
.toolbar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 0 0 16px; }
.toolbar #status { flex: 1; margin: 0; }
h2.section { font-size: 17px; margin: 18px 0 2px; }
.inline { display: flex; gap: 8px; align-items: center; margin: 0 0 8px; color: var(--text-secondary); font-size: 13px; }
.cards.compact { grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin: 0; }
.cards.compact .card { padding: 10px 12px; }
.cards.compact .card .value { font-size: 22px; }
.cards.compact .card .value.text { font-size: 15px; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin: 0 0 8px; }
.card {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
}
.card .label { color: var(--text-secondary); font-size: 13px; }
.card .value { font-size: 30px; font-weight: 600; font-variant-numeric: tabular-nums; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.grid .panel { margin: 0 0 16px; }
.chart svg { display: block; width: 100%; height: auto; }
.chart .axis { fill: var(--text-muted); font-size: 11px; }
.chart .gridline { stroke: var(--grid); stroke-width: 1; }
.chart .baseline { stroke: var(--border); stroke-width: 1; }
.chart .line { fill: none; stroke-width: 2; stroke-linejoin: round; }
.chart .s1 { stroke: var(--series-1); }
.chart .s2 { stroke: var(--series-2); }
.chart .s3 { stroke: var(--series-3); }
.chart .s4 { stroke: var(--series-4); }
.chart .bar { fill: var(--series-1); }
.chart .bar.f2 { fill: var(--series-2); }
.chart .bar.f3 { fill: var(--series-3); }
.chart .bar.f4 { fill: var(--series-4); }
.chart .value { fill: var(--text-primary); font-size: 10px; }
.chart .bar:hover { opacity: 0.8; }
.chart .cursor { stroke: var(--text-muted); stroke-width: 1; }
.chart .hit { fill: transparent; }
.legend { display: flex; flex-wrap: wrap; gap: 14px; margin: 0 0 8px; color: var(--text-secondary); font-size: 13px; }
.legend i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
.legend .k1 { background: var(--series-1); }
.legend .k2 { background: var(--series-2); }
.legend .k3 { background: var(--series-3); }
.legend .k4 { background: var(--series-4); }
.dist h3 { font-size: 12px; font-weight: 600; color: var(--text-secondary); margin: 10px 0 6px; text-transform: uppercase; letter-spacing: .04em; }
.row { display: grid; grid-template-columns: minmax(0, 9em) 1fr auto; gap: 8px; align-items: center; margin: 4px 0; font-size: 13px; }
.row .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text-primary); }
.row .track { height: 8px; border-radius: 4px; background: var(--grid); overflow: hidden; }
.row .fill { display: block; height: 100%; border-radius: 4px; background: var(--series-1); }
.row .count { font-variant-numeric: tabular-nums; color: var(--text-secondary); }
.empty { color: var(--text-muted); font-size: 13px; margin: 4px 0; }
.empty:empty { display: none; }
.pill { display: inline-block; padding: 1px 8px; border-radius: 999px; border: 1px solid var(--border); font-size: 12px; font-weight: 600; }
.pill.direct { color: var(--series-3); border-color: var(--series-3); }
.pill.relay { color: var(--series-2); border-color: var(--series-2); }
.pill.pending { color: var(--text-muted); }
.pill.mixed { color: var(--series-4); border-color: var(--series-4); }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { padding: 6px 8px; text-align: right; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
.state-Ended { color: var(--series-3); }
.state-Disconnected, .state-Failed, .state-Timeout { color: var(--error); }
th { color: var(--text-secondary); font-weight: 600; }
details summary { cursor: pointer; font-weight: 600; }
.notes ul { margin: 0; padding-left: 18px; color: var(--text-secondary); font-size: 13px; }
.notes li { margin: 4px 0; }
.tip {
  position: fixed;
  pointer-events: none;
  background: var(--surface-1);
  color: var(--text-primary);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 6px 8px;
  font-size: 12px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, .15);
  z-index: 10;
  white-space: nowrap;
}
`;

export const ADMIN_JS = String.raw`(function () {
  'use strict';
  var KEY = 'eck-dashboard-token';
  var ZONE = 'America/Sao_Paulo';
  var SVG = 'http://www.w3.org/2000/svg';
  var token = '';
  var timer = null;
  var $ = function (id) { return document.getElementById(id); };
  var regionNames = null;
  try { regionNames = new Intl.DisplayNames(['en'], { type: 'region' }); } catch (e) { regionNames = null; }
  var timeLabel = new Intl.DateTimeFormat('en-GB', { timeZone: ZONE, day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  var dayLabel = new Intl.DateTimeFormat('en-GB', { timeZone: 'UTC', day: 'numeric', month: 'short' });
  var REJECTS = {
    proto: 'Sign-in refused: different lobby protocol',
    bad_game: 'Sign-in refused: unknown game',
    bad_image: 'Sign-in refused: unknown game image',
    version_mismatch: 'Join or invite refused: different game image or build'
  };
  var PROTECTION = {
    ws_upgrade_accepted: 'WebSocket upgrades accepted',
    hello_accepted: 'Sign-ins accepted',
    hello_reconnect_deduped: 'Reconnects not counted as new sessions',
    room_host_accepted: 'Rooms created'
  };
  var ABUSE = {
    ws_upgrade_limited: 'WebSocket upgrades refused at the edge',
    ws_upgrade_ip_cap: 'WebSocket upgrades refused: too many sockets from one address',
    hello_limited: 'Sign-ins refused: too many',
    room_host_limited: 'Room creations refused',
    room_join_limited: 'Room joins refused',
    invite_limited: 'Invites refused',
    room_resume_limited: 'Seat resumes refused',
    relay_abuse_closed: 'Relay connections or relays closed for abuse',
    relay_backpressure_drop: 'Relay frames dropped for a slow seat',
    race_limited: 'Race telemetry refused'
  };
  var STATES = { open: 'Open', starting: 'Starting', racing: 'Racing' };
  var DAY_SERIES = [
    { key: 'rooms_created', label: 'Created', cls: 's1' },
    { key: 'rooms_started', label: 'Started', cls: 's2' },
    { key: 'rooms_with_guest', label: 'With a guest', cls: 's3' },
    { key: 'rooms_closed', label: 'Closed', cls: 's4' }
  ];
  var RACE_SERIES = [
    { key: 'races_started', label: 'Started', cls: 's1' },
    { key: 'races_completed', label: 'Completed', cls: 's2' },
    { key: 'races_abandoned', label: 'Abandoned', cls: 's3' }
  ];
  var TRANSPORTS = { direct: 'Direct', relay: 'Relay', mixed: 'Mixed', pending: 'Pending' };
  var OCCUPANCY = ['2/2', '2/3', '3/3', '2/4', '3/4', '4/4'];
  var LIFETIME = { under_1m: 'Under 1 minute', '1_5m': '1 to 5 minutes', '5_15m': '5 to 15 minutes', '15_60m': '15 to 60 minutes',
    over_1h: 'Over 1 hour' };
  var GAME = ${JSON.stringify(GAME_LABELS)};
  var RACE_DURATION_LABELS = { under_2m: 'Under 2 minutes', '2_4m': '2 to 4 minutes', '4_6m': '4 to 6 minutes',
    '6_10m': '6 to 10 minutes', over_10m: 'Over 10 minutes' };
  var RACES_PER_ROOM = ['1', '2', '3', '4+'];
  var COMPARE = [['sessions', 'Sessions'], ['rooms_created', 'Rooms created'], ['rooms_started', 'Rooms started'],
    ['rooms_with_guest', 'Rooms with a guest'], ['joins', 'Joins'], ['invites', 'Invites'], ['direct_selected', 'Direct selected'],
    ['relay_selected', 'Relay selected'], ['peak_online', 'Peak online'], ['peak_in_rooms', 'Peak in rooms'], ['peak_rooms', 'Peak rooms'],
    ['races_started', 'Races started'], ['races_completed', 'Races completed'], ['races_abandoned', 'Races abandoned'],
    ['rooms_with_race', 'Rooms with a race'], ['rooms_rematched', 'Rooms rematched']];

  function storedToken() {
    try { return sessionStorage.getItem(KEY) || ''; } catch (e) { return ''; }
  }
  function storeToken(value) {
    try { if (value) sessionStorage.setItem(KEY, value); else sessionStorage.removeItem(KEY); } catch (e) { /* storage blocked */ }
  }

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function svg(tag, attrs) {
    var node = document.createElementNS(SVG, tag);
    for (var k in attrs) node.setAttribute(k, attrs[k]);
    return node;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }
  function fmt(n) { return Number(n || 0).toLocaleString('en-US'); }
  function dayText(day) { return dayLabel.format(new Date(day + 'T12:00:00Z')); }
  function countryText(code) {
    if (code === 'Unknown' || code === 'Other' || !regionNames) return code;
    try { return code + ' · ' + regionNames.of(code); } catch (e) { return code; }
  }

  function AuthError() {}

  function api(path) {
    return fetch(path, { headers: { Authorization: 'Bearer ' + token }, cache: 'no-store', credentials: 'omit' })
      .then(function (r) {
        if (r.status === 401) throw new AuthError();
        if (r.status === 503) throw new Error('The dashboard token is not configured on the service.');
        if (!r.ok) throw new Error('The service answered ' + r.status + '.');
        return r.json();
      });
  }

  var tip = $('tip');
  function showTip(event, lines) {
    clear(tip);
    lines.forEach(function (line, i) { tip.appendChild(el('div', i ? '' : 'tip-head', line)); });
    tip.hidden = false;
    var x = event.clientX + 14;
    var y = event.clientY + 14;
    var w = tip.offsetWidth;
    var h = tip.offsetHeight;
    if (x + w > window.innerWidth - 8) x = event.clientX - w - 14;
    if (y + h > window.innerHeight - 8) y = event.clientY - h - 14;
    tip.style.left = Math.max(8, x) + 'px';
    tip.style.top = Math.max(8, y) + 'px';
  }
  function hideTip() { tip.hidden = true; }

  function niceMax(v) {
    if (v <= 4) return 4;
    var p = Math.pow(10, Math.floor(Math.log10(v)));
    var steps = [1, 2, 2.5, 5, 10];
    for (var i = 0; i < steps.length; i++) if (steps[i] * p >= v) return steps[i] * p;
    return 10 * p;
  }

  function frame(width, height, max, pad) {
    var root = svg('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img' });
    for (var i = 0; i <= 4; i++) {
      var y = pad.top + (height - pad.top - pad.bottom) * (1 - i / 4);
      root.appendChild(svg('line', { x1: pad.left, x2: width - pad.right, y1: y, y2: y, 'class': i ? 'gridline' : 'baseline' }));
      var label = svg('text', { x: pad.left - 6, y: y + 4, 'text-anchor': 'end', 'class': 'axis' });
      label.textContent = fmt(Math.round(max * i / 4));
      root.appendChild(label);
    }
    return root;
  }

  var SERIES = [
    { key: 'online', label: 'Online', cls: 's1', key_cls: 'k1' },
    { key: 'in_rooms', label: 'In rooms', cls: 's2', key_cls: 'k2' },
    { key: 'rooms', label: 'Rooms', cls: 's3', key_cls: 'k3' }
  ];

  function usageChart(node, history) {
    clear(node);
    var samples = history.samples;
    if (!samples.length) { node.appendChild(el('p', 'empty', 'No samples in this range yet.')); return; }
    var step = history.sample_ms;
    var byBucket = {};
    samples.forEach(function (s) { byBucket[s.bucket] = s; });
    var start = samples[0].bucket;
    var end = Math.max(samples[samples.length - 1].bucket, history.generated - (history.generated % step));
    var points = [];
    for (var b = start; b <= end; b += step) points.push(byBucket[b] || { bucket: b, online: 0, in_rooms: 0, rooms: 0 });
    var legend = el('div', 'legend');
    SERIES.forEach(function (s) {
      var item = el('span');
      item.appendChild(el('i', s.key_cls));
      item.appendChild(document.createTextNode(s.label));
      legend.appendChild(item);
    });
    node.appendChild(legend);
    var W = 900, H = 260, pad = { top: 10, right: 12, bottom: 26, left: 44 };
    var max = niceMax(Math.max.apply(null, points.map(function (p) { return Math.max(p.online, p.in_rooms, p.rooms); })));
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var n = points.length;
    var xAt = function (i) { return pad.left + (n === 1 ? plotW / 2 : plotW * i / (n - 1)); };
    var yAt = function (v) { return pad.top + plotH * (1 - v / max); };
    var ticks = Math.min(6, n);
    for (var t = 0; t < ticks; t++) {
      var i = ticks === 1 ? 0 : Math.round((n - 1) * t / (ticks - 1));
      var label = svg('text', { x: xAt(i), y: H - 8, 'text-anchor': t === 0 ? 'start' : t === ticks - 1 ? 'end' : 'middle', 'class': 'axis' });
      label.textContent = timeLabel.format(points[i].bucket);
      root.appendChild(label);
    }
    SERIES.slice().reverse().forEach(function (s) {
      var d = points.map(function (p, i) { return (i ? 'L' : 'M') + xAt(i).toFixed(1) + ' ' + yAt(p[s.key]).toFixed(1); }).join('');
      root.appendChild(svg('path', { d: d, 'class': 'line ' + s.cls }));
    });
    var cursor = svg('line', { y1: pad.top, y2: pad.top + plotH, 'class': 'cursor', visibility: 'hidden' });
    root.appendChild(cursor);
    var hit = svg('rect', { x: pad.left, y: pad.top, width: plotW, height: plotH, 'class': 'hit' });
    root.appendChild(hit);
    hit.addEventListener('mousemove', function (event) {
      var box = root.getBoundingClientRect();
      var x = (event.clientX - box.left) * W / box.width;
      var i = Math.max(0, Math.min(n - 1, Math.round((x - pad.left) / plotW * (n - 1))));
      cursor.setAttribute('x1', xAt(i));
      cursor.setAttribute('x2', xAt(i));
      cursor.setAttribute('visibility', 'visible');
      var p = points[i];
      showTip(event, [timeLabel.format(p.bucket)].concat(SERIES.map(function (s) { return s.label + ': ' + fmt(p[s.key]); })));
    });
    hit.addEventListener('mouseleave', function () { cursor.setAttribute('visibility', 'hidden'); hideTip(); });
    node.appendChild(root);
  }

  var SESSION_SERIES = [{ key: 'sessions', label: 'Sessions', cls: 's1' }];
  var VALUE_CHAR_WIDTH = 5.8;

  function sessionsChart(node, daily) {
    barChart(node, daily, SESSION_SERIES, false);
  }

  function percent(x) { return x === null ? '–' : Math.round(100 * x) + '%'; }
  function plural(n, one, many) { return fmt(n) + ' ' + (n === 1 ? one : many); }

  function barPath(x, w, base, h) {
    var r = Math.min(4, w / 2, h);
    var y = base - h;
    return 'M' + x + ' ' + base + 'V' + (y + r) + 'Q' + x + ' ' + y + ' ' + (x + r) + ' ' + y +
      'H' + (x + w - r) + 'Q' + (x + w) + ' ' + y + ' ' + (x + w) + ' ' + (y + r) + 'V' + base + 'Z';
  }

  function barChart(node, daily, series, legend) {
    clear(node);
    if (legend) {
      var key = el('div', 'legend');
      series.forEach(function (s) {
        var item = el('span');
        item.appendChild(el('i', 'k' + s.cls.slice(1)));
        item.appendChild(document.createTextNode(s.label));
        key.appendChild(item);
      });
      node.appendChild(key);
    }
    var W = 900, H = 220, pad = { top: 22, right: 12, bottom: 26, left: 44 };
    var top = 0;
    daily.forEach(function (d) { series.forEach(function (s) { top = Math.max(top, d[s.key] || 0); }); });
    var max = niceMax(top);
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var base = pad.top + plotH;
    var slot = plotW / daily.length;
    var gap = Math.min(2, slot * 0.2);
    var barSlot = (slot - gap) / series.length;
    var w = Math.max(1, series.length > 1 && barSlot > 4 ? barSlot - 1 : barSlot);
    var room = barSlot + (series.length === 1 ? gap : 0);
    daily.forEach(function (d, i) {
      series.forEach(function (s, k) {
        var v = d[s.key] || 0;
        if (v <= 0) return;
        var x = pad.left + slot * i + gap / 2 + barSlot * k;
        var h = plotH * v / max;
        root.appendChild(svg('path', { d: barPath(x, w, base, h), 'class': 'bar f' + s.cls.slice(1) }));
        var text = fmt(v);
        if (text.length * VALUE_CHAR_WIDTH > room) return;
        var value = svg('text', { x: x + w / 2, y: base - h - 4, 'text-anchor': 'middle', 'class': 'value' });
        value.textContent = text;
        root.appendChild(value);
      });
      var hit = svg('rect', { x: pad.left + slot * i, y: pad.top, width: slot, height: plotH, 'class': 'hit' });
      hit.addEventListener('mousemove', function (event) {
        showTip(event, [dayText(d.day)].concat(series.map(function (s) { return s.label + ': ' + fmt(d[s.key]); })));
      });
      hit.addEventListener('mouseleave', hideTip);
      root.appendChild(hit);
    });
    var ticks = Math.min(6, daily.length);
    for (var t = 0; t < ticks; t++) {
      var j = ticks === 1 ? 0 : Math.round((daily.length - 1) * t / (ticks - 1));
      var label = svg('text', { x: pad.left + slot * (j + 0.5), y: H - 8, 'text-anchor': 'middle', 'class': 'axis' });
      label.textContent = dayText(daily[j].day);
      root.appendChild(label);
    }
    node.appendChild(root);
  }

  function dayChart(node, daily, series) {
    barChart(node, daily, series, true);
  }

  function durationText(ms) {
    var seconds = Math.round(ms / 1000);
    if (seconds < 60) return seconds + 's';
    var minutes = Math.floor(seconds / 60);
    if (minutes < 60) return minutes + 'm ' + (seconds % 60) + 's';
    return Math.floor(minutes / 60) + 'h ' + (minutes % 60) + 'm';
  }

  function ageText(ms) {
    var minutes = Math.floor(ms / 60000);
    if (minutes < 1) return '<1 min';
    if (minutes < 60) return minutes + ' min';
    return Math.floor(minutes / 60) + ' h ' + (minutes % 60) + ' min';
  }

  function rows(node, title, items, name, tipLines) {
    var box = el('div', 'dist');
    box.appendChild(el('h3', '', title));
    if (!items.length) box.appendChild(el('p', 'empty', 'None'));
    var total = items.reduce(function (a, b) { return a + b.count; }, 0);
    var top = items.length ? items[0].count : 1;
    items.slice(0, 12).forEach(function (item) {
      var row = el('div', 'row');
      var label = name ? name(item.value) : item.value;
      row.appendChild(el('span', 'name', label)).title = label;
      var track = el('span', 'track');
      var fill = el('span', 'fill');
      fill.style.width = Math.max(2, 100 * item.count / top) + '%';
      track.appendChild(fill);
      row.appendChild(track);
      row.appendChild(el('span', 'count', fmt(item.count) + ' · ' + Math.round(100 * item.count / total) + '%'));
      if (tipLines) {
        row.addEventListener('mousemove', function (event) { showTip(event, tipLines(item, label)); });
        row.addEventListener('mouseleave', hideTip);
      }
      box.appendChild(row);
    });
    if (items.length > 12) box.appendChild(el('p', 'empty', (items.length - 12) + ' more not shown'));
    node.appendChild(box);
  }

  function distribution(id, live, range, days, name) {
    var node = $(id);
    clear(node);
    rows(node, 'Now · players online', live, name);
    rows(node, 'Sessions · last ' + days + ' days', range, name);
  }

  function transportRows(id, live, range, days) {
    var node = $(id);
    clear(node);
    var name = function (v) { return TRANSPORTS[v] || v; };
    rows(node, 'Now · seats', live, name);
    rows(node, 'Chosen · last ' + days + ' days', range, name);
  }

  function raceRows(id, items, days, name) {
    var node = $(id);
    clear(node);
    rows(node, 'Races · last ' + days + ' days', items, name);
  }

  function share(part, whole) { return whole ? (Math.round(1000 * part / whole) / 10) + '%' : '–'; }
  function characterName(id) { return GAME.characters[id] || 'Unknown'; }
  function trackName(id) { return GAME.tracks[id] || 'Unknown'; }
  function competitionName(id) { return GAME.competitions[id] || id; }
  function comboName(ids) { return ids.map(characterName).join(' + '); }
  function playersName(n) { return n + (Number(n) === 1 ? ' player' : ' players'); }
  function average(duration, completed) { return completed ? durationText(duration / completed) : '–'; }
  function changeOf(current, previous) {
    return changeText({ current: current, previous: previous, change: previous > 0 ? Math.round(1000 * (current - previous) / previous) / 10 : null });
  }

  function dataTable(id, cols, data, empty) {
    var table = $(id);
    clear(table);
    $(id + '-empty').textContent = data.length ? '' : empty;
    if (!data.length) return;
    var body = el('tbody');
    data.forEach(function (cells) {
      var tr = el('tr');
      cells.forEach(function (text) { tr.appendChild(el('td', '', text)); });
      body.appendChild(tr);
    });
    fillTable(table, cols, body);
  }

  function playerCountTable(items) {
    var total = items.reduce(function (a, r) { return a + r.races; }, 0);
    dataTable('d-player-count', ['Players', 'Races', '% of races', 'Completed', 'Completion rate', 'Abandoned', 'Average duration', 'Direct', 'Relay', 'Mixed'],
      items.map(function (r) {
        return [playersName(r.player_count), fmt(r.races), share(r.races, total), fmt(r.completed), share(r.completed, r.races), fmt(r.abandoned),
          average(r.duration_ms, r.completed), fmt(r.direct), fmt(r.relay), fmt(r.mixed)];
      }), 'No data yet.');
  }

  function outcomeTable(id, label, items, name) {
    var total = items.reduce(function (a, r) { return a + r.races; }, 0);
    dataTable(id, [label, 'Races', '% of races', 'Completed', 'Abandoned', 'Completion rate', 'Average duration'], items.map(function (r) {
      return [name(r.value), fmt(r.races), share(r.races, total), fmt(r.completed), fmt(r.abandoned), share(r.completed, r.races), average(r.duration_ms, r.completed)];
    }), 'No data yet.');
  }

  function characterPanels(history) {
    var list = history.characters;
    var picks = list.reduce(function (a, c) { return a + c.picks; }, 0);
    var top = $('d-top-characters');
    clear(top);
    rows(top, 'Picks · last ' + history.days + ' days', list.map(function (c) { return { value: c.character, count: c.picks, stats: c }; }), characterName,
      function (item, label) {
        var c = item.stats;
        return [label, 'Picks: ' + fmt(c.picks), 'Pick %: ' + share(c.picks, picks), 'Wins: ' + fmt(c.wins) + ' of ' + fmt(c.ranked), 'Win %: ' + share(c.wins, c.ranked)];
      });
    dataTable('character-picks', ['Character', 'Picks', 'Pick %', 'Races', 'Completed', 'Completion rate', 'Previous period', 'Change'], list.map(function (c) {
      return [characterName(c.character), fmt(c.picks), share(c.picks, picks), fmt(c.races), fmt(c.completed), share(c.completed, c.picks), fmt(c.previous_picks),
        changeOf(c.picks, c.previous_picks)];
    }), 'No data yet.');
    var ranked = list.filter(function (c) { return c.ranked > 0; });
    dataTable('character-finishes', ['Character', 'Ranked races', '1st', '2nd', '3rd', '4th', 'Wins', 'Win %', 'Average finish', 'Podiums', 'Last places', 'Last place %'],
      ranked.map(function (c) {
        return [characterName(c.character), fmt(c.ranked), fmt(c.positions[0]), fmt(c.positions[1]), fmt(c.positions[2]), fmt(c.positions[3]), fmt(c.wins),
          share(c.wins, c.ranked), c.average_finish.toFixed(2), fmt(c.podiums), fmt(c.last_places), share(c.last_places, c.ranked)];
      }), 'No data yet.');
  }

  function comboRows(id, title, items, whole) {
    var node = $(id);
    clear(node);
    rows(node, title, items, function (v) { return v; }, function (item, label) {
      return [label, plural(item.count, 'race', 'races'), share(item.count, whole) + ' of ' + title.toLowerCase()];
    });
  }

  function combinationPanels(history) {
    comboRows('d-pairs', 'Multiplayer races', history.pairs.map(function (p) {
      return { value: comboName([p.a, p.b]), count: p.races };
    }), history.multiplayer_races);
    [2, 3, 4].forEach(function (n) {
      var list = history.compositions[n];
      var whole = list.reduce(function (a, c) { return a + c.races; }, 0);
      comboRows('d-compositions-' + n, n + '-player races', list.map(function (c) { return { value: comboName(c.characters), count: c.races }; }), whole);
    });
    var cells = {};
    history.pairs.forEach(function (p) { cells[p.a + ':' + p.b] = cells[p.b + ':' + p.a] = p.races; });
    dataTable('pair-matrix', [''].concat(GAME.characters), history.pairs.length ? GAME.characters.map(function (name, a) {
      return [name].concat(GAME.characters.map(function (other, b) { return fmt(cells[a + ':' + b] || 0); }));
    }) : [], 'No data yet.');
  }

  function crossGroups(id, items, groupName, name) {
    var node = $(id);
    clear(node);
    var groups = {};
    var order = [];
    items.forEach(function (r) {
      if (!groups[r.b]) { groups[r.b] = []; order.push(r.b); }
      groups[r.b].push({ value: r.a, count: r.count });
    });
    if (!order.length) node.appendChild(el('p', 'empty', 'No data yet.'));
    order.forEach(function (group) { rows(node, groupName(group), groups[group], name); });
  }

  function topOf(items, key, name) {
    var best = null;
    items.forEach(function (item) { if (!best || item[key] > best[key]) best = item; });
    return best ? name(best) + ' · ' + fmt(best[key]) : '–';
  }

  function raceSummary(history) {
    var r = history.totals;
    var counts = history.player_counts;
    var seats = counts.reduce(function (a, c) { return a + c.player_count * c.races; }, 0);
    $('s-completion').textContent = share(r.races_completed, r.races_completed + r.races_abandoned);
    $('s-players').textContent = r.races_started ? (Math.round(10 * seats / r.races_started) / 10) : '–';
    $('s-popular').textContent = topOf(counts, 'races', function (c) { return playersName(c.player_count); });
    $('s-character').textContent = topOf(history.characters, 'picks', function (c) { return characterName(c.character); });
    $('s-competition').textContent = topOf(history.competitions, 'races', function (c) { return competitionName(c.value); });
    $('s-track').textContent = topOf(history.tracks, 'races', function (t) { return trackName(t.value); });
    $('s-pair').textContent = topOf(history.pairs, 'races', function (p) { return comboName([p.a, p.b]); });
    var tally = function (key) { return counts.reduce(function (a, c) { return a + c[key]; }, 0); };
    $('s-transport').textContent = counts.some(function (c) { return c.player_count > 1; }) ? [tally('direct'), tally('relay'), tally('mixed')].map(fmt).join(' / ') : '–';
  }

  function fillTable(node, cols, body) {
    var head = el('tr');
    cols.forEach(function (c) { head.appendChild(el('th', '', c)); });
    var thead = el('thead');
    thead.appendChild(head);
    node.appendChild(thead);
    node.appendChild(body);
  }

  function recentTable(recent) {
    var table = $('recent');
    clear(table);
    var list = recent.rooms;
    $('recent-title').textContent = 'Last rooms · showing ' + (recent.total > list.length
      ? list.length + ' of ' + recent.total + ' in selected range' : plural(list.length, 'room', 'rooms'));
    $('recent-empty').textContent = list.length ? '' : 'No finished rooms in this range.';
    if (!list.length) return;
    var body = el('tbody');
    list.forEach(function (r) {
      var tr = el('tr');
      tr.appendChild(el('td', '', timeLabel.format(r.ended)));
      tr.appendChild(el('td', '', durationText(r.duration_ms)));
      tr.appendChild(el('td', '', r.peak + ' / ' + r.capacity));
      tr.appendChild(el('td', '', TRANSPORTS[r.transport] || 'Unknown'));
      tr.appendChild(el('td', '', r.platforms));
      tr.appendChild(el('td', 'state-' + r.state.replace(' ', ''), r.state));
      body.appendChild(tr);
    });
    fillTable(table, ['Date and time', 'Duration', 'Peak occupancy', 'Transport', 'Platforms', 'Final state'], body);
  }

  function roomsTable(live) {
    var table = $('rooms');
    clear(table);
    var list = live.list || [];
    $('rooms-empty').textContent = list.length ? '' : 'No rooms running.';
    if (!list.length) return;
    var body = el('tbody');
    list.forEach(function (r) {
      var tr = el('tr');
      tr.appendChild(el('td', '', r.id));
      tr.appendChild(el('td', '', STATES[r.state] || r.state));
      tr.appendChild(el('td', '', r.occupied + ' / ' + r.capacity));
      tr.appendChild(el('td', '', r.connected + ' / ' + r.occupied));
      tr.appendChild(el('td', '', fmt(r.direct)));
      tr.appendChild(el('td', '', fmt(r.relay)));
      tr.appendChild(el('td', '', fmt(r.pending)));
      tr.appendChild(el('td', '', r.relay_available ? 'Available' : 'No'));
      tr.appendChild(el('td', '', r.endpoints + ' / ' + r.occupied));
      tr.appendChild(el('td', '', ageText(r.age_ms)));
      body.appendChild(tr);
    });
    fillTable(table, ['Room', 'State', 'Seats', 'Connected', 'Direct', 'Relay', 'Pending', 'Relay service', 'Endpoints', 'Age'], body);
    if (live.rooms > list.length) $('rooms-empty').textContent = (live.rooms - list.length) + ' more not shown';
  }

  function changeText(c) {
    var diff = c.current - c.previous;
    var signed = (diff > 0 ? '+' : '') + fmt(diff);
    if (c.change === null) return diff === 0 ? '–' : signed;
    return signed + ' · ' + (c.change > 0 ? '+' : '') + c.change + '%';
  }

  function comparison(history) {
    var table = $('compare');
    clear(table);
    $('compare-note').textContent = history.first + ' to ' + history.last + ' compared with the ' + history.days + ' days before.';
    var body = el('tbody');
    COMPARE.forEach(function (m) {
      var c = history.comparison[m[0]];
      var tr = el('tr');
      [m[1], fmt(c.current), fmt(c.previous), changeText(c)].forEach(function (text) { tr.appendChild(el('td', '', text)); });
      body.appendChild(tr);
    });
    fillTable(table, ['Metric', 'This period', 'Previous period', 'Change'], body);
  }

  function dailyTable(daily) {
    var table = $('daily');
    clear(table);
    var cols = [['day', 'Day'], ['sessions', 'Sessions'], ['peak_online', 'Peak online'], ['peak_in_rooms', 'Peak in rooms'],
      ['peak_rooms', 'Peak rooms'], ['rooms_created', 'Rooms created'], ['rooms_started', 'Rooms started'], ['rooms_closed', 'Rooms closed'],
      ['rooms_with_guest', 'With a guest'], ['joins', 'Joins'], ['invites', 'Invites'], ['direct_selected', 'Direct selected'],
      ['relay_selected', 'Relay selected'], ['incompatible_rejects', 'Rejects'], ['races_started', 'Races started'],
      ['races_completed', 'Races completed'], ['races_abandoned', 'Races abandoned'], ['rooms_with_race', 'Rooms with a race'],
      ['rooms_rematched', 'Rooms rematched']];
    var body = el('tbody');
    daily.slice().reverse().forEach(function (d) {
      var tr = el('tr');
      cols.forEach(function (c) { tr.appendChild(el('td', '', c[0] === 'day' ? d.day : fmt(d[c[0]]))); });
      body.appendChild(tr);
    });
    fillTable(table, cols.map(function (c) { return c[1]; }), body);
  }

  function rejects(history) {
    var node = $('rejects');
    clear(node);
    var total = history.daily.reduce(function (a, d) { return a + d.incompatible_rejects; }, 0);
    node.appendChild(el('p', 'muted', fmt(total) + ' operations refused for an incompatible protocol, game image or build in the last ' + history.days + ' days.'));
    rows(node, 'By reason', history.dimensions.reject, function (v) { return REJECTS[v] || v; });
  }

  function ordered(items, order) {
    return items.slice().sort(function (a, b) { return order.indexOf(a.value) - order.indexOf(b.value); });
  }

  function roomPanels(history) {
    var title = 'Rooms · last ' + history.days + ' days';
    var panel = function (id, items, name) {
      var node = $(id);
      clear(node);
      rows(node, title, items, name);
    };
    panel('d-capacity', history.dimensions.room_capacity, function (v) { return v + ' seats'; });
    panel('d-occupancy', ordered(history.dimensions.room_occupancy, OCCUPANCY));
    panel('d-lifetime', ordered(history.dimensions.room_lifetime, Object.keys(LIFETIME)), function (v) { return LIFETIME[v] || v; });
  }

  function counters(id, history, names, intro, empty, keep) {
    var node = $(id);
    clear(node);
    var items = history.dimensions.abuse.filter(function (item) { return keep(item.value); });
    node.appendChild(el('p', 'muted', intro + ' over the last ' + history.days + ' days, written about every 15 minutes.'));
    if (!items.length) node.appendChild(el('p', 'empty', empty + ' in this range'));
    var table = el('table');
    var body = el('tbody');
    items.forEach(function (item) {
      var tr = el('tr');
      tr.appendChild(el('td', '', names[item.value] || item.value));
      tr.appendChild(el('td', '', fmt(item.count)));
      body.appendChild(tr);
    });
    table.appendChild(body);
    node.appendChild(table);
  }

  function protection(history) {
    counters('protection', history, PROTECTION, 'Normal activity the protections let through or merged', 'No protection activity',
      function (name) { return name in PROTECTION; });
  }

  function abuse(history) {
    counters('abuse', history, ABUSE, 'Interventions: refused, limited or closed', 'No abuse events',
      function (name) { return !(name in PROTECTION); });
  }

  function render(summary, history) {
    var live = summary.live;
    $('c-online').textContent = fmt(live.online);
    $('c-in-rooms').textContent = fmt(live.in_rooms);
    $('c-advertised').textContent = fmt(live.advertised);
    $('c-active').textContent = fmt(live.active);
    $('c-direct').textContent = fmt(live.direct);
    $('c-relay').textContent = fmt(live.relay);
    $('c-pending').textContent = fmt(live.pending);
    $('c-rooms-2').textContent = fmt(live.rooms_2);
    $('c-rooms-3').textContent = fmt(live.rooms_3);
    $('c-rooms-4').textContent = fmt(live.rooms_4);
    $('c-occupied').textContent = fmt(live.occupied_seats);
    $('c-max-seats').textContent = fmt(live.max_seats);
    $('c-occupancy').textContent = live.max_seats ? percent(live.average_occupancy) : '–';
    $('c-racing').textContent = fmt(live.racing);
    var t = summary.today;
    $('t-peak').textContent = fmt(t.peak_online);
    $('t-sessions').textContent = fmt(t.sessions);
    $('t-created').textContent = fmt(t.rooms_created);
    $('t-started').textContent = fmt(t.rooms_started);
    $('t-closed').textContent = fmt(t.rooms_closed);
    $('t-joins').textContent = fmt(t.joins);
    $('t-invites').textContent = fmt(t.invites);
    $('t-direct').textContent = fmt(t.direct_selected);
    $('t-relay').textContent = fmt(t.relay_selected);
    $('t-races-started').textContent = fmt(t.races_started);
    $('t-races-completed').textContent = fmt(t.races_completed);
    $('t-races-abandoned').textContent = fmt(t.races_abandoned);
    $('today-line').textContent = 'Today (' + summary.day + '): peak ' + fmt(t.peak_in_rooms) + ' in rooms and ' +
      fmt(t.peak_rooms) + ' rooms, ' + fmt(live.connections) + ' connections open, ' +
      fmt(t.incompatible_rejects) + ' incompatibility rejects.';
    var r = history.totals;
    $('range-title').textContent = 'Selected range · last ' + history.days + ' days';
    $('r-sessions').textContent = fmt(r.sessions);
    $('r-created').textContent = fmt(r.rooms_created);
    $('r-started').textContent = fmt(r.rooms_started);
    $('r-closed').textContent = fmt(r.rooms_closed);
    $('r-guest').textContent = fmt(r.rooms_with_guest);
    $('r-joins').textContent = fmt(r.joins);
    $('r-invites').textContent = fmt(r.invites);
    $('r-rooms-direct').textContent = fmt(r.rooms_direct);
    $('r-rooms-relay').textContent = fmt(r.rooms_relay);
    $('r-rooms-pending').textContent = fmt(r.rooms_pending);
    $('r-races-started').textContent = fmt(r.races_started);
    $('r-races-completed').textContent = fmt(r.races_completed);
    $('r-races-abandoned').textContent = fmt(r.races_abandoned);
    $('r-race-duration').textContent = r.races_completed ? durationText(r.race_duration_ms / r.races_completed) : '–';
    $('r-rooms-with-race').textContent = fmt(r.rooms_with_race);
    $('r-rooms-rematched').textContent = fmt(r.rooms_rematched);
    $('r-races-per-room').textContent = r.rooms_with_race ? (Math.round(10 * r.races_in_rooms / r.rooms_with_race) / 10) : '–';
    comparison(history);
    roomsTable(live);
    recentTable(history.recent);
    usageChart($('usage'), history);
    sessionsChart($('sessions'), history.daily);
    dayChart($('rooms-by-day'), history.daily, DAY_SERIES);
    roomPanels(history);
    distribution('d-platform', live.platform, history.dimensions.platform, history.days);
    distribution('d-version', live.version, history.dimensions.version, history.days,
      function (v) { return /^[0-9]/.test(v) ? 'v' + v : v; });
    distribution('d-country', live.country, history.dimensions.country, history.days, countryText);
    transportRows('d-transport', live.transport, history.dimensions.transport, history.days);
    dayChart($('races-by-day'), history.daily, RACE_SERIES);
    playerCountTable(history.player_counts);
    raceSummary(history);
    raceRows('d-race-context', history.dimensions.race_context, history.days, function (v) { return v === 'online' ? 'Online room' : 'Local'; });
    raceRows('d-race-transport', history.dimensions.race_transport, history.days, function (v) { return TRANSPORTS[v] || v; });
    raceRows('d-race-duration', ordered(history.dimensions.race_duration_bucket, Object.keys(RACE_DURATION_LABELS)), history.days,
      function (v) { return RACE_DURATION_LABELS[v] || v; });
    raceRows('d-races-per-room', ordered(history.dimensions.races_per_room, RACES_PER_ROOM), history.days,
      function (v) { return v + (v === '1' ? ' race' : ' races'); });
    characterPanels(history);
    combinationPanels(history);
    outcomeTable('competition-table', 'Competition', history.competitions, competitionName);
    outcomeTable('track-table', 'Track', history.tracks, trackName);
    crossGroups('d-character-competition', history.race_cross.character_competition, competitionName, characterName);
    crossGroups('d-character-track', history.race_cross.character_track, trackName, characterName);
    dailyTable(history.daily);
    rejects(history);
    protection(history);
    abuse(history);
    $('status').textContent = 'Updated ' + timeLabel.format(summary.generated) + ' (Sao Paulo)';
  }

  function load() {
    var days = $('range').value;
    $('status').textContent = 'Loading…';
    return Promise.all([api('/admin/api/summary'), api('/admin/api/history?days=' + encodeURIComponent(days) + '&recent=' + encodeURIComponent($('recent-limit').value))])
      .then(function (r) { render(r[0], r[1]); })
      .catch(function (error) {
        if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
        $('status').textContent = error && error.message ? error.message : 'Could not load the metrics.';
      });
  }

  function showDashboard() {
    $('login').hidden = true;
    $('dash').hidden = false;
    load();
    if (timer) clearInterval(timer);
    timer = setInterval(function () { if (!document.hidden) load(); }, 60000);
  }

  function signOut(message) {
    token = '';
    storeToken('');
    if (timer) clearInterval(timer);
    timer = null;
    $('dash').hidden = true;
    $('login').hidden = false;
    $('token').value = '';
    $('login-error').textContent = message || '';
    $('token').focus();
  }

  $('login').addEventListener('submit', function (event) {
    event.preventDefault();
    token = $('token').value.trim();
    $('token').value = '';
    if (!token) return;
    storeToken($('remember').checked ? token : '');
    $('login-error').textContent = '';
    showDashboard();
  });
  $('refresh').addEventListener('click', load);
  $('range').addEventListener('change', load);
  $('recent-limit').addEventListener('change', load);
  $('signout').addEventListener('click', function () { signOut(''); });

  token = storedToken();
  if (token) { $('remember').checked = true; showDashboard(); }
}());
`;
