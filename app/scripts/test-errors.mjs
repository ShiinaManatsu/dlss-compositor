import assert from 'node:assert/strict';
import {RR_FG_CREATE_FAILED, userErrorMessage} from '../src/errors.ts';
import {fgAllowed, nativeResAllowed, rrAllowed} from '../src/dcp.ts';
import {phaseLabel, planRamWindows, ramWindowBudget} from '../src/ramWindows.ts';

const blob =
  '{"event": "error", "message": "drive R: is not mounted; Stage 3 will not create a RAM disk", "missing": false} drive R: is not mounted; Stage 3 will not create a RAM disk';
assert.equal(userErrorMessage(blob), 'R: 未挂载');
assert.equal(userErrorMessage(blob).includes('{'), false);
assert.equal(userErrorMessage(blob).includes('not mounted'), false);
assert.equal(userErrorMessage('{"event":"error","message":"drive S: is not mounted"}'), 'S: 未挂载');
assert.equal(userErrorMessage('未找到卷标 DLSSRAM 的 RAM 盘。Stage 3 禁止自动建盘。'), '未找到 DLSSRAM');
assert.equal(userErrorMessage('R: 未挂载'), 'R: 未挂载');
assert.equal(userErrorMessage('请安装 ImDisk Toolkit'), '请安装 ImDisk Toolkit');
assert.equal(userErrorMessage('export-dir must be on the working disk, not the RAM disk'), '导出目录必须在工作盘，不能写到 RAM 盘');

assert.equal(rrAllowed('CYCLES'), true);
assert.equal(nativeResAllowed('CYCLES', 'sr'), true);
assert.equal(nativeResAllowed('CYCLES', 'rr'), false, 'RR disables native-res buffers');
assert.equal(nativeResAllowed('EEVEE', 'sr'), false);
assert.equal(nativeResAllowed('BLENDER_EEVEE_NEXT', 'sr'), false);
assert.equal(fgAllowed('sr'), true);
assert.equal(fgAllowed('rr'), true);
assert.equal(RR_FG_CREATE_FAILED, 'RR+FG 创建失败');
assert.equal(userErrorMessage(RR_FG_CREATE_FAILED), 'RR+FG 创建失败');
assert.equal(RR_FG_CREATE_FAILED.includes('未接线'), false);
assert.equal(RR_FG_CREATE_FAILED.includes('RrFgPipeline'), false);

assert.equal(phaseLabel('beauty', 830, 835), '渲染 0830–0835');
assert.equal(phaseLabel('guides', 830, 835), '引导 0830–0835');
assert.equal(phaseLabel('rebuild', 830, 835), '重建 0830–0835');

const eightGig = 8 * 1024 ** 3;
const budget = ramWindowBudget(eightGig);
assert.equal(budget, eightGig - 1024 ** 3);
const windows = planRamWindows(830, 841, 8 * 1024 ** 2, budget, 6);
assert.ok(windows.length >= 2, 'forced max_frames must span two windows');
assert.deepEqual(windows[0], [830, 835]);
assert.equal(windows.at(-1)[1], 841);

console.log('test-errors ok');
