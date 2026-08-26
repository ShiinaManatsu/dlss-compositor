import assert from 'node:assert/strict';
import {
  jobCount,
  mapBlenderFrame,
  mapEngineFrame,
  playheadRatio,
  emptyFrameTiming,
  startFrameTiming,
  recordFrameAdvance,
  rollingAverageMs,
  formatCompactDuration,
  formatEtaLine
} from '../src/progress.ts';

assert.equal(jobCount(830, 850), 21);
assert.equal(mapEngineFrame(4, 830, 850), 833);
assert.equal(mapEngineFrame(833, 830, 850), 833);
assert.equal(mapEngineFrame(0, 830, 850), null);
assert.equal(mapEngineFrame(3010, 830, 850), null);
assert.equal(mapBlenderFrame(4, 830, 850), null);
assert.equal(mapBlenderFrame(830, 830, 850), 830);
assert.equal(mapBlenderFrame(3010, 830, 850), null);
assert.equal(playheadRatio(830, 830, 850), 0);
assert.equal(playheadRatio(850, 830, 850), 1);
assert.ok(playheadRatio(833, 830, 850) > playheadRatio(830, 830, 850));

assert.equal(formatCompactDuration(2100), '2.1s');
assert.equal(formatCompactDuration(21000), '21s');
assert.equal(formatCompactDuration(90_000), '1.5m');
assert.equal(formatCompactDuration(14 * 60 * 1000), '14m');
assert.equal(formatCompactDuration(63 * 60 * 1000), '1h 3m');

let timing = startFrameTiming(1000);
assert.equal(formatEtaLine(timing, 50), null, 'no ETA before first completed frame');
assert.equal(rollingAverageMs(timing), null);

timing = recordFrameAdvance(timing, 300, 300, 1500);
assert.equal(formatEtaLine(timing, 50), null, 'same-frame tick is not a sample');

timing = recordFrameAdvance(timing, 300, 301, 3100);
assert.equal(timing.lastMs, 2100);
assert.equal(rollingAverageMs(timing), 2100);
assert.equal(formatEtaLine(timing, 39), '2.1s/帧 · 剩余 1.4m');

timing = recordFrameAdvance(timing, 301, 303, 7300);
assert.equal(timing.lastMs, 2100);
assert.equal(timing.samplesMs.length, 3);
assert.equal(formatEtaLine(emptyFrameTiming(), 10), null);

console.log('test-progress ok');
