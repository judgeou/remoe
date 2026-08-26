import assert from 'node:assert/strict';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import WebSocket from 'ws';
import { createStore, openDatabase } from '../lib/database.mjs';
import { tokenHash } from '../lib/security.mjs';

const port = 18080 + Math.floor(Math.random() * 1000);
const testDirectory = mkdtempSync(join(tmpdir(), 'remoe-signal-test-'));
const databasePath = join(testDirectory, 'remoe.db');
const testUserId = 'test_user_123456789';
const testHostId = 'test_host_123456789';
const testHostToken = 'host_token_abcdefghijklmnopqrstuvwxyz123456';
const testWebSession = 'web_session_abcdefghijklmnopqrstuvwxyz123456';
const testCredentialId = 'credential_abcdefghijklmnopqrstuvwxyz123456';
let serverProcess;

before(async () => {
  const database = openDatabase(databasePath);
  const store = createStore(database);
  database.prepare('INSERT INTO users(id, created_at) VALUES(?, ?)').run(testUserId, Date.now());
  store.createHost(testHostId, testUserId, 'Test managed Host', tokenHash(testHostToken));
  store.createSession(tokenHash(testWebSession), testUserId, Date.now() + 60_000);
  store.addPasskey({
    credentialId: testCredentialId,
    userId: testUserId,
    publicKey: Buffer.from([1, 2, 3]),
    counter: 0,
    transports: JSON.stringify(['internal']),
    backupEligible: 0,
    backupState: 0,
    createdAt: Date.now(),
  });
  database.close();
  serverProcess = spawn(process.execPath, ['server.mjs'], {
    cwd: new URL('..', import.meta.url),
    env: {
      ...process.env,
      REMOE_SIGNAL_PORT: String(port),
      REMOE_DATABASE_PATH: databasePath,
      REMOE_ORIGIN: `http://localhost:${port}`,
      REMOE_RP_ID: 'localhost',
    },
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  await once(serverProcess.stdout, 'data');
});

after(async () => {
  const exited = serverProcess && serverProcess.exitCode === null ? once(serverProcess, 'exit') : null;
  serverProcess?.kill('SIGTERM');
  if (exited) await exited;
  rmSync(testDirectory, { recursive: true, force: true });
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

test('assigns an authenticated managed Host without exposing an invite to the user', async () => {
  const host = new WebSocket(`ws://127.0.0.1:${port}/host`,
    `remoe-host.${testHostId}.${testHostToken}`);
  const registration = once(host, 'message');
  await once(host, 'open');
  assert.equal((await registration)[0].toString(), 'registered');

  const accountResponse = await fetch(`http://127.0.0.1:${port}/api/account`, {
    headers: { cookie: `remoe_session=${testWebSession}` },
  });
  const account = await accountResponse.json();
  assert.equal(account.authenticated, true);
  assert.equal(account.accountId, testUserId);
  assert.equal(account.hosts[0].online, true);

  const connectResponse = await fetch(
    `http://127.0.0.1:${port}/api/hosts/${testHostId}/connect`, {
      method: 'POST',
      headers: {
        cookie: `remoe_session=${testWebSession}`,
        origin: `http://localhost:${port}`,
        'content-type': 'application/json',
      },
      body: '{}',
    });
  assert.equal(connectResponse.status, 200);
  const { invite } = await connectResponse.json();
  const session = new URL(invite).hash.slice(1);
  assert.match(session, /^[A-Za-z0-9_-]{20,64}$/);

  const client = await connectRegistered('client', session);
  const hostMessage = once(host, 'message');
  client.send(Buffer.from([7, 8, 9]));
  const [hostData, hostBinary] = await hostMessage;
  assert.equal(hostBinary, true);
  assert.deepEqual(hostData, Buffer.from([7, 8, 9]));
  client.close();
  host.close();
});

test('pairs a new Host to an authenticated account', async () => {
  const pairingSocket = new WebSocket(`ws://127.0.0.1:${port}/host`, 'remoe-pair.new');
  const pairingMessage = once(pairingSocket, 'message');
  await once(pairingSocket, 'open');
  const code = (await pairingMessage)[0].toString().replace('pairing:', '');
  assert.match(code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);

  const pairedMessage = once(pairingSocket, 'message');
  const response = await fetch(`http://127.0.0.1:${port}/api/hosts/pair`, {
    method: 'POST',
    headers: {
      cookie: `remoe_session=${testWebSession}`,
      origin: `http://localhost:${port}`,
      'content-type': 'application/json',
    },
    body: JSON.stringify({ code, name: 'New paired Host' }),
  });
  assert.equal(response.status, 200);
  const paired = (await pairedMessage)[0].toString();
  assert.match(paired, /^paired:[A-Za-z0-9_-]{16,64}:[A-Za-z0-9_-]{32,128}$/);
});

test('repairs an existing Host by keeping its id and rotating its token', async () => {
  const repairSocket = new WebSocket(`ws://127.0.0.1:${port}/host`,
    `remoe-pair.${testHostId}.${testHostToken}`);
  const pairingMessage = once(repairSocket, 'message');
  await once(repairSocket, 'open');
  const code = (await pairingMessage)[0].toString().replace('pairing:', '');

  const pairedMessage = once(repairSocket, 'message');
  const response = await fetch(`http://127.0.0.1:${port}/api/hosts/pair`, {
    method: 'POST',
    headers: {
      cookie: `remoe_session=${testWebSession}`,
      origin: `http://localhost:${port}`,
      'content-type': 'application/json',
    },
    body: JSON.stringify({ code, name: 'Repaired Host' }),
  });
  assert.equal(response.status, 200);
  const [prefix, repairedId, repairedToken] = (await pairedMessage)[0].toString().split(':');
  assert.equal(prefix, 'paired');
  assert.equal(repairedId, testHostId);
  assert.notEqual(repairedToken, testHostToken);

  const host = new WebSocket(`ws://127.0.0.1:${port}/host`,
    `remoe-host.${repairedId}.${repairedToken}`);
  const registration = once(host, 'message');
  await once(host, 'open');
  assert.equal((await registration)[0].toString(), 'registered');
  host.close();
});

test('issues WebAuthn registration and discoverable login challenges', async () => {
  const headers = {
    origin: `http://localhost:${port}`,
    'content-type': 'application/json',
  };
  const registrationResponse = await fetch(
    `http://127.0.0.1:${port}/api/auth/register/options`, {
      method: 'POST', headers, body: '{}',
    });
  assert.equal(registrationResponse.status, 200);
  assert.match(registrationResponse.headers.get('set-cookie'), /remoe_ceremony=/);
  const registration = await registrationResponse.json();
  assert.equal(registration.rp.id, 'localhost');
  assert.equal(registration.authenticatorSelection.authenticatorAttachment, 'platform');
  assert.equal(registration.authenticatorSelection.residentKey, 'preferred');
  assert.equal(registration.authenticatorSelection.requireResidentKey, false);
  assert.equal(registration.authenticatorSelection.userVerification, 'required');
  assert.ok(registration.challenge.length >= 32);

  const compatibilityResponse = await fetch(
    `http://127.0.0.1:${port}/api/auth/register/options`, {
      method: 'POST', headers, body: JSON.stringify({ compatibilityMode: true }),
    });
  assert.equal(compatibilityResponse.status, 200);
  const compatibility = await compatibilityResponse.json();
  assert.equal(compatibility.authenticatorSelection.authenticatorAttachment, 'platform');
  assert.equal('residentKey' in compatibility.authenticatorSelection, false);
  assert.equal(compatibility.authenticatorSelection.requireResidentKey, false);
  assert.equal(compatibility.authenticatorSelection.userVerification, 'required');

  const loginResponse = await fetch(`http://127.0.0.1:${port}/api/auth/login/options`, {
    method: 'POST', headers, body: '{}',
  });
  assert.equal(loginResponse.status, 200);
  const authentication = await loginResponse.json();
  assert.equal(authentication.rpId, 'localhost');
  assert.equal(authentication.userVerification, 'required');
  assert.deepEqual(authentication.allowCredentials, []);

  const localLoginResponse = await fetch(`http://127.0.0.1:${port}/api/auth/login/options`, {
    method: 'POST', headers,
    body: JSON.stringify({ credentialIds: [testCredentialId, 'invalid'] }),
  });
  assert.equal(localLoginResponse.status, 200);
  const localAuthentication = await localLoginResponse.json();
  assert.deepEqual(localAuthentication.allowCredentials, [{
    id: testCredentialId,
    type: 'public-key',
    transports: ['internal'],
  }]);

  const unknownLoginResponse = await fetch(`http://127.0.0.1:${port}/api/auth/login/options`, {
    method: 'POST', headers,
    body: JSON.stringify({ credentialIds: ['credential_unknown_123456789'] }),
  });
  assert.equal(unknownLoginResponse.status, 400);
  assert.deepEqual(await unknownLoginResponse.json(), { error: 'Passkey is not registered' });
});

test('refreshes an authenticated login as a browser-session cookie', async () => {
  const response = await fetch(`http://127.0.0.1:${port}/api/account`, {
    headers: { cookie: `remoe_session=${testWebSession}` },
  });
  assert.equal(response.status, 200);
  const setCookie = response.headers.get('set-cookie');
  assert.match(setCookie, /^remoe_session=/);
  assert.match(setCookie, /HttpOnly/);
  assert.doesNotMatch(setCookie, /Max-Age|Expires/i);
});
