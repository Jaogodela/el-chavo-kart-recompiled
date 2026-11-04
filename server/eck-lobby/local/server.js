#!/usr/bin/env node
// The Worker's hub over node:http and node:sqlite, for the automated tests and optional development. Players use the
// deployed service.
//   node server/eck-lobby/local/server.js [--host 127.0.0.1] [--port 8791] [--db <file>|:memory:] [--verbose]
// ECK_LOBBY=ws://127.0.0.1:8791/ points a game at it. Loopback clients are exempt from the per-address limits.
import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { Hub } from '../src/hub.js';
import { LIMITS, healthReport } from '../src/protocol.js';
import { LocalConn, LocalHost, sqliteStore } from './host.js';
import { upgrade } from './ws.js';

const describe = (text) => text.replace(/"token":"[0-9a-f]+"/g, '"token":"<hidden>"').slice(0, 300);

export function startLobbyServer({ host: address = '127.0.0.1', port = 8791, db = ':memory:', verbose = false,
  log = () => {}, images } = {}) {
  const host = new LocalHost(sqliteStore(db), { log });
  const hub = new Hub(host, { local: true, images });
  host.onAlarm = () => hub.alarm();
  let stopped = false;

  const server = createServer((req, res) => {
    const path = new URL(req.url, 'http://local').pathname;
    const found = path === '/' || path === '/health';
    res.statusCode = found ? 200 : 404;
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify(found ? healthReport() : { error: 'not_found' }));
  });

  server.on('upgrade', (req, socket) => {
    const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '';
    const first = hub.admit(ip);
    if (!first) return socket.end('HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n');
    let conn = null;
    const transport = upgrade(req, socket, {
      maxMessage: LIMITS.frameBytes * 2,
      onText(text) {
        if (stopped) return;
        if (text === 'ping') return transport.send('pong');
        if (verbose) log(`<- ${first.cid} ${describe(text)}`);
        hub.message(conn, text);
      },
      onBinary(bytes) {
        if (!stopped) hub.relay(conn, bytes);
      },
      onClose() {
        if (!conn || stopped) return;
        hub.closed(conn);
        host.conns.delete(conn);
      },
    });
    if (!transport) return socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    const send = transport.send;
    transport.send = (text) => {
      if (verbose) log(`-> ${first.cid} ${text.slice(0, 300)}`);
      send(text);
    };
    conn = new LocalConn(transport);
    conn.setAttachment(first);
    host.conns.add(conn);
  });

  return new Promise((resolve) => {
    server.listen(port, address, () => {
      resolve({
        port: server.address().port,
        hub,
        host,
        close: () => new Promise((done) => {
          stopped = true;
          if (host.timer) clearTimeout(host.timer);
          for (const conn of host.conns) conn.close(1001, 'server stopping');
          server.close(() => {
            host.sql.close();
            done();
          });
          server.closeAllConnections?.();
        }),
      });
    });
  });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      host: { type: 'string', default: '127.0.0.1' },
      port: { type: 'string', default: process.env.PORT || '8791' },
      db: { type: 'string', default: ':memory:' },
      verbose: { type: 'boolean', default: false },
    },
  });
  const log = (text) => console.log(`${new Date().toISOString().slice(11, 23)} ${text}`);
  const started = await startLobbyServer({ host: values.host, port: Number(values.port), db: values.db,
    verbose: values.verbose, log });
  log(`eck-lobby listening; use ECK_LOBBY=ws://${values.host}:${started.port}/ (database ${values.db}); Ctrl+C stops it`);
}
