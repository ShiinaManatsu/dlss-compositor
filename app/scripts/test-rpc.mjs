import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {PassThrough} from 'node:stream';
import {RpcError, StdioRpcClient} from '../electron/rpc.ts';

function fakeProc() {
  const proc = new EventEmitter();
  proc.stdin = new PassThrough();
  proc.stdout = new PassThrough();
  proc.exitCode = null;
  proc.killed = false;
  return proc;
}

const closed = fakeProc();
const client = new StdioRpcClient(closed);
closed.stdin.end();
assert.equal(client.writable(), false);

let uncaught = 0;
const onUncaught = () => {
  uncaught += 1;
};
process.prependListener('uncaughtException', onUncaught);
try {
  await assert.rejects(() => client.request('stop', {}, 200), (err) => {
    assert.ok(err instanceof RpcError);
    return true;
  });
  await new Promise((r) => setTimeout(r, 40));
  assert.equal(uncaught, 0);
} finally {
  process.removeListener('uncaughtException', onUncaught);
}

const live = fakeProc();
const liveClient = new StdioRpcClient(live);
assert.equal(liveClient.writable(), true);
liveClient.close();
assert.equal(liveClient.writable(), false);
await assert.rejects(() => liveClient.request('start_job', {}, 200), (err) => err instanceof RpcError);

function encodeRpc(payload) {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'ascii');
  return Buffer.concat([header, body]);
}

const matchProc = fakeProc();
const matchClient = new StdioRpcClient(matchProc);
matchProc.stdin.on('data', () => {
  matchProc.stdout.write(encodeRpc({jsonrpc: '2.0', id: 99, result: {ok: true, stale: true}}));
  matchProc.stdout.write(encodeRpc({jsonrpc: '2.0', id: 1, result: {ok: true, matched: true}}));
});
const matched = await matchClient.request('progress', {});
assert.equal(matched.matched, true);
assert.equal(matched.stale, undefined);

const lateProc = fakeProc();
const lateClient = new StdioRpcClient(lateProc);
await assert.rejects(
  () => lateClient.request('progress', {}, 80),
  (err) => err instanceof RpcError && err.message === 'RPC timeout'
);
lateProc.stdout.write(encodeRpc({jsonrpc: '2.0', id: 1, result: {ok: true, stale: true}}));
await new Promise((r) => setTimeout(r, 30));
lateProc.stdin.once('data', () => {
  lateProc.stdout.write(encodeRpc({jsonrpc: '2.0', id: 2, result: {ok: true, second: true}}));
});
const second = await lateClient.request('stop', {});
assert.equal(second.second, true);
assert.equal(second.stale, undefined);

console.log('test-rpc ok');
