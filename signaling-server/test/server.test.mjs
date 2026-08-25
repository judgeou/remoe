import assert from 'node:assert/strict';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { after, before, test } from 'node:test';
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

async function connectRegistered(role, session = 'test_session_1234') {
  const socket = connect(role, session);
  const opened = once(socket, 'open');
  const registration = once(socket, 'message');
  await opened;
  const [message, isBinary] = await registration;
  assert.equal(isBinary, false);
  assert.equal(message.toString(), 'registered');
  return socket;
}

async function expectRejected(role, session, expected) {
  const socket = connect(role, session);
  const response = once(socket, 'message');
  const closed = once(socket, 'close');
  await once(socket, 'open');
  const [message, isBinary] = await response;
  assert.equal(isBinary, false);
  assert.equal(message.toString(), `error:${expected}`);
  await closed;
}

test('relays binary messages in both directions', async () => {
  const host = await connectRegistered('host');
  const client = await connectRegistered('client');

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

test('rejects a client when the host invite is absent', async () => {
  await expectRejected('client', 'missing_session_1234', 'invite-not-found');
});

test('rejects duplicate roles', async () => {
  const first = await connectRegistered('host', 'duplicate_session');
  await expectRejected('host', 'duplicate_session', 'host-in-use');
  first.close();
});

test('expires the invite when its host disconnects', async () => {
  const session = 'reusable_session_1234';
  const firstHost = await connectRegistered('host', session);
  const firstClient = await connectRegistered('client', session);
  const clientClosed = once(firstClient, 'close');
  firstHost.close();
  await once(firstHost, 'close');
  await clientClosed;

  await expectRejected('client', session, 'invite-not-found');

  const secondHost = await connectRegistered('host', session);
  const secondClient = await connectRegistered('client', session);
  const freshMessage = once(secondClient, 'message');
  secondHost.send(Buffer.from('fresh'));
  const [data] = await freshMessage;
  assert.equal(data.toString(), 'fresh');
  secondHost.close();
  secondClient.close();
});
