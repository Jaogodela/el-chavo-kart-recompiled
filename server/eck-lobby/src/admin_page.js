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
    <div class="cards">
      <div class="card" id="k-online"><span class="label">Online now</span><span id="c-online" class="value">–</span></div>
      <div class="card" id="k-in-rooms"><span class="label">Players in rooms</span><span id="c-in-rooms" class="value">–</span></div>
      <div class="card" id="k-rooms"><span class="label">Open rooms</span><span id="c-rooms" class="value">–</span></div>
      <div class="card" id="k-peak"><span class="label">Peak today</span><span id="c-peak" class="value">–</span></div>
      <div class="card" id="k-unique-today"><span class="label">Unique players today</span><span id="c-unique-today" class="value">–</span></div>
      <div class="card" id="k-unique-range"><span id="c-unique-range-label" class="label">Unique players in period</span><span id="c-unique-range" class="value">–</span></div>
      <div class="card" id="k-races-today"><span class="label">Races today</span><span id="c-races-today" class="value">–</span></div>
      <div class="card" id="k-races-range"><span id="c-races-range-label" class="label">Races in period</span><span id="c-races-range" class="value">–</span></div>
    </div>
    <p id="cards-note" class="muted"></p>
    <h2 class="section">Period comparison</h2>
    <div class="panel">
      <p id="compare-note" class="muted"></p>
      <div class="scroll"><table id="compare"></table></div>
    </div>
    <h2 class="section">Multiplayer activity</h2>
    <div class="panel">
      <p id="activity-note" class="muted"></p>
      <div class="cards" id="activity"></div>
      <div id="d-party-size"></div>
    </div>
    <h2 class="section">Players per race</h2>
    <div class="panel">
      <p class="muted">Counted when each race starts. Local split-screen races count every player on the PC.</p>
      <div class="grid wide">
        <div id="d-player-count"></div>
        <div id="d-race-context"></div>
      </div>
    </div>
    <h2 class="section">Characters</h2>
    <div class="panel"><div id="d-character"></div></div>
    <h2 class="section">Character combinations</h2>
    <div class="panel">
      <p class="muted">Multiplayer races only. The same characters count as one combination whatever the seat order.</p>
      <div id="d-combination"></div>
    </div>
    <h2 class="section">Tracks</h2>
    <div class="panel"><div id="d-track"></div></div>
    <h2 class="section">Competitions</h2>
    <div class="panel"><div id="d-competition"></div></div>
    <h2 class="section">Historical activity</h2>
    <div class="panel">
      <h2>Usage over time</h2>
      <p class="muted">Highest value in each 15-minute interval; kept for 90 days.</p>
      <div id="usage" class="chart"></div>
    </div>
    <div class="panel"><h2>Unique players by day</h2><div id="h-unique" class="chart"></div></div>
    <div class="panel"><h2>Sessions by day</h2><div id="h-sessions" class="chart"></div></div>
    <div class="panel"><h2>Races started by day</h2><div id="h-races-started" class="chart"></div></div>
    <div class="panel"><h2>Races completed by day</h2><div id="h-races-completed" class="chart"></div></div>
    <div class="panel"><h2>Rooms created by day</h2><div id="h-rooms" class="chart"></div></div>
    <div class="panel"><h2>Peak online by day</h2><div id="h-peak-online" class="chart"></div></div>
    <div class="panel">
      <h2>Daily totals</h2>
      <div class="scroll"><table id="daily"></table></div>
    </div>
    <details id="technical" class="panel">
      <summary>Technical details</summary>
      <div class="cards compact" id="technical-cards"></div>
      <div class="grid">
        <div class="panel"><h2>Platform</h2><div id="d-platform"></div></div>
        <div class="panel"><h2>Port version</h2><div id="d-version"></div></div>
        <div class="panel"><h2>Country</h2><div id="d-country"></div></div>
      </div>
    </details>
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
    --error: #f97066;
  }
}
:root[data-theme="dark"] {
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
h2.section { font-size: 17px; margin: 24px 0 10px; }
.up { color: var(--series-3); }
.down { color: var(--error); }
.muted { color: var(--text-muted); margin: 0 0 12px; font-size: 13px; }
.muted:empty { display: none; }
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
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; margin: 0 0 8px; }
.cards.compact { grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin: 0 0 12px; }
.cards.compact .card { padding: 10px 12px; }
.cards.compact .card .value { font-size: 22px; }
.card {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px 16px;
  display: flex;
  flex-direction: column;
}
.card[data-help] { cursor: help; }
.card[data-help]:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.card .label { color: var(--text-secondary); font-size: 13px; }
.card .value { font-size: 30px; font-weight: 600; font-variant-numeric: tabular-nums; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.grid.wide { grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
.grid .panel { margin: 0 0 16px; }
.chart svg { display: block; width: 100%; height: auto; }
.chart .axis { fill: var(--text-muted); font-size: 11px; }
.chart .gridline { stroke: var(--grid); stroke-width: 1; }
.chart .baseline { stroke: var(--border); stroke-width: 1; }
.chart .line { fill: none; stroke-width: 2; stroke-linejoin: round; }
.chart .s1 { stroke: var(--series-1); }
.chart .s2 { stroke: var(--series-2); }
.chart .s3 { stroke: var(--series-3); }
.chart .bar { fill: var(--series-1); }
.chart .bar:hover { opacity: 0.8; }
.chart .value-label { fill: var(--text-secondary); font-size: 10px; font-variant-numeric: tabular-nums; }
.chart .cursor { stroke: var(--text-muted); stroke-width: 1; }
.chart .hit { fill: transparent; }
.legend { display: flex; flex-wrap: wrap; gap: 14px; margin: 0 0 8px; color: var(--text-secondary); font-size: 13px; }
.legend i { display: inline-block; width: 14px; height: 3px; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
.legend .k1 { background: var(--series-1); }
.legend .k2 { background: var(--series-2); }
.legend .k3 { background: var(--series-3); }
.dist h3 { font-size: 12px; font-weight: 600; color: var(--text-secondary); margin: 10px 0 6px; text-transform: uppercase; letter-spacing: .04em; }
.dist:first-child h3 { margin-top: 0; }
.row { display: grid; grid-template-columns: minmax(0, 9em) 1fr auto; gap: 8px; align-items: center; margin: 4px 0; font-size: 13px; }
.dist.wide .row { grid-template-columns: minmax(0, 22em) 1fr auto; }
.dist.big .row { grid-template-columns: minmax(0, 9em) 1fr auto; margin: 10px 0; font-size: 15px; }
.dist.big .track { height: 14px; border-radius: 7px; }
.dist.big .fill { border-radius: 7px; }
.row .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text-primary); }
.row .track { height: 8px; border-radius: 4px; background: var(--grid); overflow: hidden; }
.row .fill { display: block; height: 100%; border-radius: 4px; background: var(--series-1); }
.row .count { font-variant-numeric: tabular-nums; color: var(--text-secondary); }
.empty { color: var(--text-muted); font-size: 13px; margin: 4px 0; }
.empty:empty { display: none; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { padding: 6px 8px; text-align: right; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-secondary); font-weight: 600; }
details summary { cursor: pointer; font-weight: 600; }
details[open] summary { margin-bottom: 12px; }
@media (max-width: 560px) {
  .dist.wide .row { grid-template-columns: minmax(0, 11em) 1fr auto; }
}
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
  max-width: 320px;
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
  var GAME = ${JSON.stringify(GAME_LABELS)};
  var COMPETITIONS = Object.keys(GAME.competitions);
  var TOP_COMBINATIONS = 15;
  var ACCEPTED = ['ws_upgrade_accepted', 'hello_accepted', 'hello_reconnect_deduped', 'room_host_accepted'];
  var CARD_HELP = {
    'k-online': 'Players connected to the online service right now, counted once per account',
    'k-in-rooms': 'Players occupying a seat in an online room right now',
    'k-rooms': 'Rooms that exist right now, waiting for players or racing',
    'k-peak': 'Highest number of players online at the same time today',
    'k-unique-today': 'A unique player is counted once today, however often they reconnect, through an anonymous key',
    'k-unique-range': 'A unique player is counted once for the whole selected period, even when they played on several days',
    'k-races-today': 'Races started today, online and local, as reported by the game',
    'k-races-range': 'Races started in the selected period, online and local, as reported by the game'
  };
  var COMPARE = [['unique_players', 'Unique players'], ['sessions', 'Sessions'], ['rooms_created', 'Rooms created'],
    ['joins', 'Successful joins'], ['races_started', 'Races started'], ['races_completed', 'Races completed'],
    ['races_2p', '2-player races'], ['races_3p', '3-player races'], ['races_4p', '4-player races']];
  var DAILY = [['day', 'Day'], ['unique_players', 'Unique players'], ['sessions', 'Sessions'], ['peak_online', 'Peak online'],
    ['rooms_created', 'Rooms created'], ['joins', 'Joins'], ['races_started', 'Races started'], ['races_completed', 'Races completed']];
  var SERIES = [
    { key: 'online', label: 'Online', cls: 's1', key_cls: 'k1' },
    { key: 'in_rooms', label: 'In rooms', cls: 's2', key_cls: 'k2' },
    { key: 'rooms', label: 'Open rooms', cls: 's3', key_cls: 'k3' }
  ];

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
  function share(part, whole) {
    if (!(whole > 0)) return '–';
    var p = 100 * part / whole;
    return part > 0 && p < 0.5 ? '<1%' : Math.round(p) + '%';
  }
  function playersName(n) { return n + (Number(n) === 1 ? ' player' : ' players'); }
  function characterName(id) { return GAME.characters[id]; }
  function trackName(id) { return GAME.tracks[id]; }
  function competitionName(id) { return GAME.competitions[id]; }

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

  function helpOn(node, help) {
    node.setAttribute('data-help', help);
    node.setAttribute('tabindex', '0');
    node.addEventListener('mousemove', function (event) { showTip(event, [help]); });
    node.addEventListener('mouseleave', hideTip);
    node.addEventListener('focus', function () {
      var box = node.getBoundingClientRect();
      showTip({ clientX: box.left, clientY: box.bottom - 14 }, [help]);
    });
    node.addEventListener('blur', hideTip);
  }
  Object.keys(CARD_HELP).forEach(function (id) { helpOn($(id), CARD_HELP[id]); });

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

  function barChart(node, daily, key, metric) {
    clear(node);
    var W = 900, H = 200, pad = { top: 22, right: 12, bottom: 26, left: 44 };
    var labelled = daily.length <= 30;
    var max = niceMax(Math.max.apply(null, daily.map(function (d) { return d[key]; })));
    var root = frame(W, H, max, pad);
    var plotW = W - pad.left - pad.right;
    var plotH = H - pad.top - pad.bottom;
    var slot = plotW / daily.length;
    var gap = Math.min(2, slot * 0.2);
    daily.forEach(function (d, i) {
      var h = plotH * d[key] / max;
      var x = pad.left + slot * i + gap / 2;
      var w = Math.max(1, slot - gap);
      if (h > 0) {
        var r = Math.min(4, w / 2, h);
        var y = pad.top + plotH - h;
        var path = 'M' + x + ' ' + (pad.top + plotH) + 'V' + (y + r) + 'Q' + x + ' ' + y + ' ' + (x + r) + ' ' + y +
          'H' + (x + w - r) + 'Q' + (x + w) + ' ' + y + ' ' + (x + w) + ' ' + (y + r) + 'V' + (pad.top + plotH) + 'Z';
        root.appendChild(svg('path', { d: path, 'class': 'bar' }));
        if (labelled) {
          var value = svg('text', { x: x + w / 2, y: y - 5, 'text-anchor': 'middle', 'class': 'value-label' });
          value.textContent = fmt(d[key]);
          root.appendChild(value);
        }
      }
      var hit = svg('rect', { x: pad.left + slot * i, y: pad.top, width: slot, height: plotH, 'class': 'hit' });
      hit.addEventListener('mousemove', function (event) { showTip(event, [dayText(d.day), metric + ': ' + fmt(d[key])]); });
      hit.addEventListener('mouseleave', hideTip);
      root.appendChild(hit);
    });
    var ticks = Math.min(6, daily.length);
    for (var t = 0; t < ticks; t++) {
      var i = ticks === 1 ? 0 : Math.round((daily.length - 1) * t / (ticks - 1));
      var label = svg('text', { x: pad.left + slot * (i + 0.5), y: H - 8, 'text-anchor': 'middle', 'class': 'axis' });
      label.textContent = dayText(daily[i].day);
      root.appendChild(label);
    }
    node.appendChild(root);
  }

  // Horizontal bars with count and share of the items given. options: limit, empty, tip(item, label), style (dist class).
  function rows(node, title, items, name, options) {
    options = options || {};
    var box = el('div', 'dist' + (options.style ? ' ' + options.style : ''));
    if (title) box.appendChild(el('h3', '', title));
    var total = items.reduce(function (a, b) { return a + b.count; }, 0);
    if (!(total > 0)) {
      box.appendChild(el('p', 'empty', options.empty || 'No data yet'));
      node.appendChild(box);
      return;
    }
    var top = Math.max.apply(null, items.map(function (i) { return i.count; }));
    var limit = options.limit || 12;
    items.slice(0, limit).forEach(function (item) {
      var row = el('div', 'row');
      var label = name ? name(item.value) : item.value;
      row.appendChild(el('span', 'name', label)).title = label;
      var track = el('span', 'track');
      var fill = el('span', 'fill');
      fill.style.width = (item.count > 0 ? Math.max(2, 100 * item.count / top) : 0) + '%';
      track.appendChild(fill);
      row.appendChild(track);
      row.appendChild(el('span', 'count', fmt(item.count) + ' · ' + share(item.count, total)));
      row.addEventListener('mousemove', function (event) {
        showTip(event, options.tip ? options.tip(item, label, total) : [label, fmt(item.count) + ' · ' + share(item.count, total)]);
      });
      row.addEventListener('mouseleave', hideTip);
      box.appendChild(row);
    });
    if (items.length > limit) box.appendChild(el('p', 'empty', (items.length - limit) + ' more not shown'));
    node.appendChild(box);
  }

  function table(node, head, body) {
    clear(node);
    var thead = el('thead');
    var tr = el('tr');
    head.forEach(function (h) { tr.appendChild(el('th', '', h)); });
    thead.appendChild(tr);
    node.appendChild(thead);
    var tbody = el('tbody');
    body.forEach(function (cells) {
      var row = el('tr');
      cells.forEach(function (c) { row.appendChild(typeof c === 'string' ? el('td', '', c) : c); });
      tbody.appendChild(row);
    });
    node.appendChild(tbody);
  }

  function changeCell(c) {
    var td = el('td');
    if (c.change === null) { td.textContent = c.current > 0 ? 'new' : '–'; return td; }
    var diff = c.current - c.previous;
    td.className = diff > 0 ? 'up' : diff < 0 ? 'down' : '';
    td.textContent = (diff > 0 ? '+' : '') + fmt(diff) + ' (' + (c.change > 0 ? '+' : '') + c.change + '%)';
    return td;
  }

  function shiftDay(day, offset) {
    var date = new Date(day + 'T12:00:00Z');
    date.setUTCDate(date.getUTCDate() + offset);
    return date.toISOString().slice(0, 10);
  }

  function comparison(history) {
    $('compare-note').textContent = history.first + ' to ' + history.last + ' compared with ' + shiftDay(history.first, -history.days) +
      ' to ' + shiftDay(history.first, -1) + '.';
    table($('compare'), ['Metric', 'This period', 'Previous period', 'Change'], COMPARE.map(function (m) {
      var c = history.comparison[m[0]];
      return [m[1], fmt(c.current), fmt(c.previous), changeCell(c)];
    }));
  }

  function card(label, value, help) {
    var node = el('div', 'card');
    node.appendChild(el('span', 'label', label));
    node.appendChild(el('span', 'value', value));
    if (help) helpOn(node, help);
    return node;
  }

  function counted(items, ids) {
    var by = {};
    items.forEach(function (i) { by[i.value] = i.count; });
    return ids.map(function (id) { return { value: id, count: by[id] || 0 }; });
  }
  function byCount(items) {
    return items.map(function (item, i) { return { item: item, i: i }; })
      .sort(function (a, b) { return b.item.count - a.item.count || a.i - b.i; })
      .map(function (x) { return x.item; });
  }

  function multiplayer(history) {
    var t = history.totals;
    var node = $('activity');
    clear(node);
    node.appendChild(card('Rooms created', fmt(t.rooms_created), 'Online rooms created in the selected period'));
    node.appendChild(card('Successful joins', fmt(t.joins),
      'Players entering a room. Not unique players: the same player can join several rooms'));
    node.appendChild(card('Races started', fmt(t.races_started), 'Races the game reported as started, online and local'));
    node.appendChild(card('Races completed', fmt(t.races_completed), 'Races the game reported as finished'));
    node.appendChild(card('Completion rate', share(t.races_completed, t.races_completed + t.races_abandoned),
      'Completed races as a share of races that ended, finished or abandoned. Races still running are not counted'));
    $('activity-note').textContent = 'Last ' + history.days + ' days. Party size counts rooms that have ended, by the most players they held at once.';
    var sizes = history.party_sizes.map(function (s) { return { value: s.players, count: s.count }; });
    var holder = $('d-party-size');
    clear(holder);
    rows(holder, 'Party size · rooms', sizes, function (v) { return v === 1 ? 'Solo (no guest joined)' : playersName(v); },
      { empty: 'No rooms have ended in this range yet.' });
  }

  function playersPerRace(history) {
    var counts = counted(history.player_counts.map(function (c) { return { value: c.player_count, count: c.races }; }), [1, 2, 3, 4]);
    var races = $('d-player-count');
    clear(races);
    rows(races, 'Races · last ' + history.days + ' days', counts, playersName,
      { style: 'big', empty: 'No races have been reported in this range yet.' });
    var where = $('d-race-context');
    clear(where);
    rows(where, 'Where races were played', counted(history.dimensions.race_context, ['online', 'local']),
      function (v) { return v === 'online' ? 'Online room' : 'Local'; }, { empty: 'No races have been reported in this range yet.' });
  }

  function characters(history) {
    var picks = {};
    history.characters.forEach(function (c) { picks[c.character] = c; });
    var items = GAME.characters.map(function (name, id) { return { value: id, count: picks[id] ? picks[id].picks : 0 }; });
    var node = $('d-character');
    clear(node);
    rows(node, 'Characters selected · last ' + history.days + ' days', items, characterName, {
      limit: items.length,
      empty: 'No races with known characters in this range yet.',
      tip: function (item, label, total) {
        return [label, 'Picks: ' + fmt(item.count), 'Share: ' + share(item.count, total), 'Races: ' + fmt(picks[item.value] ? picks[item.value].races : 0)];
      }
    });
  }

  function combinations(history) {
    var all = [];
    [2, 3, 4].forEach(function (n) {
      (history.compositions[n] || []).forEach(function (c) {
        if (!c.characters.every(function (id) { return characterName(id) !== undefined; })) return;
        all.push({ value: c.characters.map(characterName).join(' + '), count: c.races, players: n });
      });
    });
    var node = $('d-combination');
    clear(node);
    rows(node, 'Top ' + TOP_COMBINATIONS + ' combinations · last ' + history.days + ' days', byCount(all), null, {
      limit: TOP_COMBINATIONS,
      style: 'wide',
      empty: 'No multiplayer races with known characters in this range yet.',
      tip: function (item, label, total) {
        return [label, playersName(item.players), fmt(item.count) + ' ' + (item.count === 1 ? 'race' : 'races') + ' · ' + share(item.count, total)];
      }
    });
  }

  function raceShare(id, title, ids, outcomes, name, empty) {
    var node = $(id);
    clear(node);
    var races = counted(outcomes.map(function (o) { return { value: o.value, count: o.races }; }), ids);
    rows(node, title, byCount(races), name, { limit: ids.length, empty: empty });
  }

  function tracks(history) {
    var ids = GAME.tracks.map(function (name, id) { return String(id); });
    raceShare('d-track', 'Races per track · last ' + history.days + ' days', ids, history.tracks,
      function (v) { return trackName(v); }, 'No races with a known track in this range yet.');
  }

  function competitions(history) {
    raceShare('d-competition', 'Races per competition · last ' + history.days + ' days', COMPETITIONS, history.competitions,
      competitionName, 'No races have been reported in this range yet.');
  }

  function dailyTable(daily) {
    table($('daily'), DAILY.map(function (c) { return c[1]; }), daily.slice().reverse().map(function (d) {
      return DAILY.map(function (c) { return c[0] === 'day' ? d.day : fmt(d[c[0]]); });
    }));
  }

  function distribution(id, items, days, name) {
    var node = $(id);
    clear(node);
    rows(node, 'Sessions · last ' + days + ' days', items, name);
  }

  function technical(history) {
    var t = history.totals;
    var refused = history.dimensions.abuse.reduce(function (a, i) { return ACCEPTED.indexOf(i.value) < 0 ? a + i.count : a; }, 0);
    var cards = $('technical-cards');
    clear(cards);
    cards.appendChild(card('Relay rooms', fmt(t.rooms_relay)));
    cards.appendChild(card('Direct rooms', fmt(t.rooms_direct)));
    cards.appendChild(card('Rooms with no path', fmt(t.rooms_pending)));
    cards.appendChild(card('Protocol rejects', fmt(t.incompatible_rejects)));
    cards.appendChild(card('Refused by limits', fmt(refused)));
    distribution('d-platform', history.dimensions.platform, history.days);
    distribution('d-version', history.dimensions.version, history.days, function (v) { return /^[0-9]/.test(v) ? 'v' + v : v; });
    distribution('d-country', history.dimensions.country, history.days, countryText);
  }

  function uniqueNote(history) {
    var from = history.players_first_day;
    $('cards-note').textContent = !from ? 'Unique players start counting at the first sign-in after this version is deployed.'
      : from > history.first ? 'Unique players are counted from ' + from + '; earlier days show 0.' : '';
  }

  function render(summary, history) {
    var live = summary.live;
    var today = summary.today;
    $('c-online').textContent = fmt(live.online);
    $('c-in-rooms').textContent = fmt(live.in_rooms);
    $('c-rooms').textContent = fmt(live.rooms);
    $('c-peak').textContent = fmt(today.peak_online);
    $('c-unique-today').textContent = fmt(today.unique_players);
    $('c-unique-range-label').textContent = 'Unique players · ' + history.days + ' days';
    $('c-unique-range').textContent = fmt(history.totals.unique_players);
    $('c-races-today').textContent = fmt(today.races_started);
    $('c-races-range-label').textContent = 'Races · ' + history.days + ' days';
    $('c-races-range').textContent = fmt(history.totals.races_started);
    uniqueNote(history);
    comparison(history);
    multiplayer(history);
    playersPerRace(history);
    characters(history);
    combinations(history);
    tracks(history);
    competitions(history);
    usageChart($('usage'), history);
    barChart($('h-unique'), history.daily, 'unique_players', 'Unique players');
    barChart($('h-sessions'), history.daily, 'sessions', 'Sessions');
    barChart($('h-races-started'), history.daily, 'races_started', 'Races started');
    barChart($('h-races-completed'), history.daily, 'races_completed', 'Races completed');
    barChart($('h-rooms'), history.daily, 'rooms_created', 'Rooms created');
    barChart($('h-peak-online'), history.daily, 'peak_online', 'Peak online');
    dailyTable(history.daily);
    technical(history);
    $('status').textContent = 'Updated ' + timeLabel.format(summary.generated) + ' (Sao Paulo)';
  }

  function load() {
    var days = $('range').value;
    $('status').textContent = 'Loading…';
    return Promise.all([api('/admin/api/summary'), api('/admin/api/history?days=' + encodeURIComponent(days) + '&recent=1')])
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
  $('signout').addEventListener('click', function () { signOut(''); });

  token = storedToken();
  if (token) { $('remember').checked = true; showDashboard(); }
}());
`;
