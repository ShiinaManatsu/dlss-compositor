import assert from 'node:assert/strict';
import {
  DEFAULT_FILENAME_TEMPLATE,
  DCP_KIND,
  clampSplitters,
  defaultDcpPath,
  jobTokenFromBlend,
  nextOutputVersion,
  parseDcp,
  projectTitle,
  resolveFilenameTemplate,
  nativeResAllowed,
  fgAllowed,
  parseFg,
  cyclesDeviceMeta,
  cyclesDenoiseMeta,
  outputFrameCount,
  fgRpcValue,
  rrAllowed,
  serializeDcp
} from '../src/dcp.ts';

const parsed = parseDcp(
  JSON.stringify({
    blend: 'F:\\Films\\shot.blend',
    mode: 'rr',
    pins: {percentage: true, samples: true},
    percentage: 67,
    samples: 32,
    quality: 'QUALITY',
    outputWidth: 1920,
    outputHeight: 1080,
    outputFormat: 'prores_422hq',
    exportDir: 'D:\\out',
    filenameTemplate: '{job}_{mode}_v{version}',
    outputVersion: 3,
    splitters: {queue: 300, inspector: 400}
  })
);
assert.equal(parsed.ok, true);
if (!parsed.ok) throw new Error(parsed.error);
assert.equal(parsed.project.kind, DCP_KIND);
assert.equal(parsed.project.mode, 'rr');
assert.equal(parsed.project.pins.percentage, true);
assert.equal(parsed.project.pins.frameStart, false);
assert.equal(parsed.project.filenameTemplate, '{job}_{mode}_v{version}');
assert.equal(parsed.project.outputVersion, 3);
assert.equal(parsed.project.splitters.queue, 300);
assert.equal(parsed.project.nativeResBuffers, false);
assert.equal(parsed.project.fg, 0);

const withNative = parseDcp(
  JSON.stringify({
    blend: 'F:\\Films\\shot.blend',
    mode: 'sr',
    nativeResBuffers: true,
    percentage: 58
  })
);
assert.equal(withNative.ok, true);
if (!withNative.ok) throw new Error(withNative.error);
assert.equal(withNative.project.nativeResBuffers, true);

const withFg = parseDcp(
  JSON.stringify({
    blend: 'F:\\Films\\shot.blend',
    mode: 'sr',
    fg: '2x'
  })
);
assert.equal(withFg.ok, true);
if (!withFg.ok) throw new Error(withFg.error);
assert.equal(withFg.project.fg, 2);

const withFgMultiplier = parseDcp(
  JSON.stringify({
    blend: 'F:\\Films\\shot.blend',
    mode: 'sr',
    fg_multiplier: 4
  })
);
assert.equal(withFgMultiplier.ok, true);
if (!withFgMultiplier.ok) throw new Error(withFgMultiplier.error);
assert.equal(withFgMultiplier.project.fg, 4);

assert.equal(parseFg(undefined), 0);
assert.equal(parseFg('off'), 0);
assert.equal(parseFg('关'), 0);
assert.equal(parseFg('2x'), 2);
assert.equal(parseFg(3), 3);
assert.equal(parseFg('7x'), 0);
assert.equal(fgRpcValue(0), undefined);
assert.equal(fgRpcValue(2), '2x');
assert.equal(outputFrameCount(4, 0), 4);
assert.equal(outputFrameCount(4, 2), 7);
assert.equal(outputFrameCount(4, 3), 10);
assert.equal(fgAllowed('sr'), true);
assert.equal(fgAllowed('rr'), true);

const roundtrip = parseDcp(serializeDcp(parsed.project));
assert.equal(roundtrip.ok, true);
if (!roundtrip.ok) throw new Error(roundtrip.error);
assert.deepEqual(roundtrip.project, parsed.project);
assert.equal(roundtrip.project.fg, 0);

const fgRoundtrip = parseDcp(serializeDcp(withFg.project));
assert.equal(fgRoundtrip.ok, true);
if (!fgRoundtrip.ok) throw new Error(fgRoundtrip.error);
assert.equal(fgRoundtrip.project.fg, 2);
assert.match(serializeDcp(withFg.project), /"fg": 2/);

assert.equal(parseDcp('not-json').ok, false);
assert.equal(parseDcp('{}').ok, false);
assert.equal(parseDcp(JSON.stringify({blend: 'a.blend'})).ok, false);
assert.equal(parseDcp(JSON.stringify({mode: 'sr'})).ok, false);
assert.equal(parseDcp(JSON.stringify({kind: 'other', blend: 'a.blend', mode: 'sr'})).ok, false);

const incomplete = parseDcp('{"blend":"a.blend"}');
assert.equal(incomplete.ok, false);

assert.equal(rrAllowed('CYCLES'), true);
assert.equal(rrAllowed('EEVEE'), false);
assert.equal(rrAllowed('EEVEE_NEXT'), false);
assert.equal(rrAllowed('BLENDER_EEVEE_NEXT'), false);
assert.equal(nativeResAllowed('CYCLES', 'sr'), true);
assert.equal(nativeResAllowed('CYCLES', 'rr'), false);
assert.equal(nativeResAllowed('EEVEE', 'sr'), false);
assert.equal(cyclesDeviceMeta('GPU', 'OPTIX'), '设备 GPU/OptiX');
assert.equal(cyclesDeviceMeta('CPU', ''), '设备 CPU');
assert.equal(cyclesDenoiseMeta({denoiser: 'OPENIMAGEDENOISE', denoiser_label: 'OIDN'}), '降噪 OIDN');
assert.equal(
  cyclesDenoiseMeta({
    denoiser: 'OPENIMAGEDENOISE',
    denoiser_label: 'OIDN',
    preview_denoiser: 'OPTIX',
    preview_denoiser_label: 'OptiX'
  }),
  '降噪 OIDN · 视口 OptiX'
);
assert.equal(cyclesDenoiseMeta({denoiser: ''}), '');

assert.equal(clampSplitters({queue: 10, inspector: 9000}).queue, 180);
assert.equal(clampSplitters({queue: 10, inspector: 9000}).inspector, 520);
assert.equal(nextOutputVersion(3), 4);
assert.equal(nextOutputVersion(0), 2);
assert.equal(defaultDcpPath('F:\\Films\\shot.blend'), 'F:\\Films\\shot.dcp');
assert.equal(projectTitle('F:\\Films\\shot.dcp'), 'shot');
assert.equal(projectTitle(null), 'DLSS Compositor');
assert.equal(DEFAULT_FILENAME_TEMPLATE.includes('{job}'), true);

assert.equal(jobTokenFromBlend('F:\\Films\\5.render_material_remaster.blend'), '5.render_material_remaster');

const mov = resolveFilenameTemplate('{job}_{mode}_v{version}', {
  job: '5.render_material_remaster',
  mode: 'rr',
  version: 2,
  format: 'prores_422hq'
});
assert.equal(mov.ok, true);
if (!mov.ok) throw new Error(mov.error);
assert.equal(mov.template, '5.render_material_remaster_rr_v2.mov');

const seqDefault = resolveFilenameTemplate('{job}_{mode}_v{version}', {
  job: 'shot',
  mode: 'sr',
  version: 1,
  format: 'exr'
});
assert.equal(seqDefault.ok, true);
if (!seqDefault.ok) throw new Error(seqDefault.error);
assert.equal(seqDefault.template, 'shot_sr_v1_{frame:04d}.exr');

const seqFrame = resolveFilenameTemplate('{job}_{mode}_v{version}_{frame}', {
  job: 'shot',
  mode: 'sr',
  version: 3,
  format: 'png_16'
});
assert.equal(seqFrame.ok, true);
if (!seqFrame.ok) throw new Error(seqFrame.error);
assert.equal(seqFrame.template, 'shot_sr_v3_{frame:04d}.png');

const empty = resolveFilenameTemplate('   ', {job: 'shot', mode: 'sr', version: 1, format: 'prores_422hq'});
assert.equal(empty.ok, false);

const unknown = resolveFilenameTemplate('{job}_{foo}', {job: 'shot', mode: 'sr', version: 1, format: 'prores_422hq'});
assert.equal(unknown.ok, false);
if (!unknown.ok) assert.match(unknown.error, /无法解析/);

const frameOnlyMov = resolveFilenameTemplate('{frame}', {job: 'shot', mode: 'sr', version: 1, format: 'prores_422hq'});
assert.equal(frameOnlyMov.ok, false);

console.log('test-dcp ok');
