import assert from 'node:assert/strict';
import { test } from 'node:test';

import { PLATFORMS, metricsDay, platformCategory } from '../src/metrics.js';
import { identity, makeHub } from './harness.js';

const dimension = (env, name) => Object.fromEntries(env.host.sql.all(
  'SELECT value, count FROM metrics_daily_dimensions WHERE day = ? AND dimension = ?', metricsDay(env.clock.now), name)
  .map((r) => [r.value, r.count]));

test('the platforms the clients report are the ones the dashboard counts', () => {
  assert.deepEqual(PLATFORMS, ['Windows', 'Linux', 'Steam Deck']);
  for (const platform of PLATFORMS) assert.equal(platformCategory(platform), platform);
  for (const unknown of ['linux', 'LINUX', 'Ubuntu', '', undefined]) assert.equal(platformCategory(unknown), 'Other');
});

test('Windows and Linux clients sign in to the same service and are counted apart', () => {
  const env = makeHub();
  for (const [n, platform] of [[1, 'Windows'], [2, 'Linux'], [3, 'Linux']]) {
    const c = env.connect(identity(n), { platform, version: '0.1.0' });
    assert.equal(c.hello().code, 'ok');
  }
  assert.deepEqual(dimension(env, 'platform'), { Windows: 1, Linux: 2 });
  assert.deepEqual(dimension(env, 'version'), { '0.1.0': 3 });
  const history = env.hub.metricsHistory(30, 20);
  assert.ok(history.dimensions.platform, 'the dashboard history carries the platform dimension');
});
