import assert from 'node:assert/strict';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { after, before, test } from 'node:test';
import { setTimeout as delay } from 'node:timers/promises';
import WebSocket from 'ws';

const port = 18080 + Math.floor(Math.random() * 1000);
let serverProcess;

before(async () => {
  serverProcess = spawn(process.execPath, ['server.mjs'], {
    cwd: new URL('..', import.meta.url),
    env: { ...process.env, REMOE_SIGNAL_PORT: String(port) },
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  await once(serverProcess.stdout, 'data');
});

after(() => {
  serverProcess?.kill('SIGTERM');
});

function connect(role, session = 'test_session_1234') {
  return new WebSocket(`ws://127.0.0.1:${port}/signal?session=${session}&role=${role}`);
}

test('relays binary messages in both directions', async () => {
  const host = connect('host');
  const client = connect('client');
  await Promise.all([once(host, 'open'), once(client, 'open')]);

  const hostMessage = once(host, 'message');
  client.send(Buffer.from([1, 2, 3]));
  const [hostData, hostBinary] = await hostMessage;
  assert.equal(hostBinary, true);
  assert.deepEqual(hostData, Buffer.from([1, 2, 3]));

  const clientMessage = once(client, 'message');
  host.send(Buffer.from([4, 5, 6]));
  const [clientData, clientBinary] = await clientMessage;
  assert.equal(clientBinary, true);
  assert.deepEqual(clientData, Buffer.from([4, 5, 6]));
  host.close();
  client.close();
});

test('queues messages until the peer connects', async () => {
  const client = connect('client', 'queued_session_1234');
  await once(client, 'open');
  client.send(Buffer.from('offer'));

  const host = connect('host', 'queued_session_1234');
  const message = once(host, 'message');
  await once(host, 'open');
  const [data] = await message;
  assert.equal(data.toString(), 'offer');
  host.close();
  client.close();
});

test('rejects duplicate roles', async () => {
  const first = connect('host', 'duplicate_session');
  await once(first, 'open');
  const second = connect('host', 'duplicate_session');
  const [error] = await once(second, 'error');
  assert.match(error.message, /409/);
  first.close();
});

test('discards queued signaling when its sender disconnects', async () => {
  const session = 'reusable_session_1234';
  const firstHost = connect('host', session);
  await once(firstHost, 'open');
  firstHost.send(Buffer.from('stale'));
  firstHost.close();
  await once(firstHost, 'close');
  await delay(20);

  const client = connect('client', session);
  await once(client, 'open');
  let received = false;
  client.once('message', () => { received = true; });
  await delay(50);
  assert.equal(received, false);

  const secondHost = connect('host', session);
  await once(secondHost, 'open');
  const freshMessage = once(client, 'message');
  secondHost.send(Buffer.from('fresh'));
  const [data] = await freshMessage;
  assert.equal(data.toString(), 'fresh');
  secondHost.close();
  client.close();
});
