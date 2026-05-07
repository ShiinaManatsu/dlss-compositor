/**
 * Unit-style tests for parseNdjsonLine — no Electron, no browser.
 * Validates the NDJSON event parser used to ingest dlss-compositor stdout.
 */

import { test, expect } from '@playwright/test';
import { parseNdjsonLine } from '../electron/progress-parser';

test.describe('parseNdjsonLine', () => {
  test('returns null for non-JSON line', () => {
    expect(parseNdjsonLine('Processing frame 1/10')).toBeNull();
    expect(parseNdjsonLine('')).toBeNull();
    expect(parseNdjsonLine('{ invalid json')).toBeNull();
    expect(parseNdjsonLine('null')).toBeNull();
    expect(parseNdjsonLine('123')).toBeNull();
  });

  test('returns null for JSON without event field', () => {
    expect(parseNdjsonLine('{"foo":"bar"}')).toBeNull();
    expect(parseNdjsonLine('{"event":42}')).toBeNull();
    expect(parseNdjsonLine('[]')).toBeNull();
  });

  test('parses started event', () => {
    const result = parseNdjsonLine(
      '{"event":"started","total":10,"input_dir":"/in","output_dir":"/out"}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('started');
    if (result?.event === 'started') {
      expect(result.total).toBe(10);
      expect(result.input_dir).toBe('/in');
      expect(result.output_dir).toBe('/out');
    }
  });

  test('parses frame_started event', () => {
    const result = parseNdjsonLine(
      '{"event":"frame_started","n":1,"total":10,"timestamp_ms":1234}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('frame_started');
    if (result?.event === 'frame_started') {
      expect(result.n).toBe(1);
      expect(result.total).toBe(10);
      expect(result.timestamp_ms).toBe(1234);
    }
  });

  test('parses frame_written event', () => {
    const result = parseNdjsonLine(
      '{"event":"frame_written","n":1,"path":"/out/frame_0001.exr","timestamp_ms":1234}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('frame_written');
    if (result?.event === 'frame_written') {
      expect(result.n).toBe(1);
      expect(result.path).toBe('/out/frame_0001.exr');
    }
  });

  test('parses banner event', () => {
    const result = parseNdjsonLine(
      '{"event":"banner","sdk":"310.5.3","engine":"0.2.1","sr_available":true,"fg_available":false,"max_multi_frame":0}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('banner');
    if (result?.event === 'banner') {
      expect(result.sdk).toBe('310.5.3');
      expect(result.engine).toBe('0.2.1');
      expect(result.sr_available).toBe(true);
      expect(result.fg_available).toBe(false);
      expect(result.max_multi_frame).toBe(0);
    }
  });

  test('parses error event', () => {
    const result = parseNdjsonLine(
      '{"event":"error","frame":3,"message":"failed to read EXR"}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('error');
    if (result?.event === 'error') {
      expect(result.frame).toBe(3);
      expect(result.message).toBe('failed to read EXR');
    }
  });

  test('parses complete event', () => {
    const result = parseNdjsonLine(
      '{"event":"complete","total":10,"duration_ms":5000,"errors":0}'
    );
    expect(result).not.toBeNull();
    expect(result?.event).toBe('complete');
    if (result?.event === 'complete') {
      expect(result.total).toBe(10);
      expect(result.duration_ms).toBe(5000);
      expect(result.errors).toBe(0);
    }
  });

  test('tolerates extra unknown fields on a known event', () => {
    const result = parseNdjsonLine(
      '{"event":"frame_started","n":1,"total":10,"timestamp_ms":1234,"extra":"ignored"}'
    );
    expect(result?.event).toBe('frame_started');
  });
});
