import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { RELAY, SUPPORTED_IMAGES, encodeRelayFrame, parseRelayFrame, playerIdFor, tokenHash } from '../src/protocol.js';

const FOREIGN = ['ss', 'f2t'].join('');
const root = join(import.meta.dirname, '..');
const hex = (text) => Uint8Array.from(text.match(/../g).map((byte) => parseInt(byte, 16)));

test('the relay frame layout is the one PROTOCOL.md documents', () => {
  const bytes = encodeRelayFrame(0x89abcdef, 7, 2, hex('010203'), 0);
  assert.deepEqual(bytes, hex('454b0101' + '89abcdef' + '00000007' + '02' + '00' + '0003' + '010203'));
  assert.deepEqual(parseRelayFrame(bytes), { type: 1, token: 0x89abcdef, seq: 7, dest: 2, length: 3 });
  const broadcast = encodeRelayFrame(1, 0xffffffff, 0xff, hex('aa'), 3);
  assert.deepEqual(broadcast, hex('454b0101' + '00000001' + 'ffffffff' + 'ff' + '03' + '0001' + 'aa'));
  assert.deepEqual([RELAY.headerBytes, RELAY.maxPayload, RELAY.broadcast, RELAY.version, RELAY.data], [16, 1472, 255, 1, 1]);
});

test('identity derivation matches the documented algorithm, computed independently', () => {
  const sha = (text) => createHash('sha256').update(text).digest('hex');
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  for (const token of ['a'.repeat(64), '0123456789abcdef'.repeat(4), 'f'.repeat(64)]) {
    const digest = sha(`eck-player-id:${token}`);
    const chars = Array.from({ length: 8 }, (_, i) => alphabet[Number.parseInt(digest.slice(2 * i, 2 * i + 2), 16) % 32]);
    assert.equal(playerIdFor(token), `${chars.slice(0, 4).join('')}-${chars.slice(4).join('')}`);
    assert.equal(tokenHash(token), sha(`eck-token:${token}`));
  }
  assert.equal(playerIdFor('a'.repeat(64)), playerIdFor('a'.repeat(64)));
});

test('the supported image allowlist names the title image', () => {
  assert.ok(SUPPORTED_IMAGES.includes('eck-usa-1db4e96b'));
});

test('the service directory has no leftovers from other games', () => {
  const found = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      if (name === 'node_modules' || name === '.wrangler') continue;
      const path = join(dir, name);
      if (statSync(path).isDirectory()) walk(path);
      else if (readFileSync(path, 'utf8').toLowerCase().includes(FOREIGN) || name.toLowerCase().includes(FOREIGN)) found.push(path);
    }
  };
  walk(root);
  assert.deepEqual(found, []);
});
