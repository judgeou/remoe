import { afterEach, beforeEach, expect, test, vi } from 'vitest';

vi.mock('@simplewebauthn/browser', () => ({
  startAuthentication: vi.fn(),
  startRegistration: vi.fn(),
}));

import { startAuthentication, startRegistration } from '@simplewebauthn/browser';
import { login, loginWithOtherPasskey, logout, recoverAccount } from '../src/api.ts';

const credentialId = 'credential_1234567890';

beforeEach(() => {
  const values = new Map();
  vi.stubGlobal('localStorage', {
    getItem: vi.fn((key) => values.get(key) ?? null),
    setItem: vi.fn((key, value) => values.set(key, String(value))),
  });
});

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

test('recovery logs in with an existing passkey when registration detects a duplicate', async () => {
  startRegistration.mockRejectedValueOnce(Object.assign(
    new Error('The authenticator was previously registered'),
    { code: 'ERROR_AUTHENTICATOR_PREVIOUSLY_REGISTERED' },
  ));
  startAuthentication.mockResolvedValueOnce({ id: credentialId, response: {} });

  const requests = [];
  vi.stubGlobal('fetch', vi.fn(async (path, init) => {
    requests.push({ path, body: init?.body ? JSON.parse(init.body) : null });
    if (path === '/api/recovery/options') {
      return Response.json({
        challenge: 'registration-challenge',
        rp: { id: 'example.com', name: 'remoe' },
        user: { id: 'user-id', name: 'user', displayName: 'user' },
        pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
        excludeCredentials: [{ id: credentialId, type: 'public-key' }],
      });
    }
    if (path === '/api/auth/login/options') {
      return Response.json({ challenge: 'login-challenge', rpId: 'example.com' });
    }
    if (path === '/api/auth/login/verify') return Response.json({ verified: true });
    throw new Error(`Unexpected request: ${path}`);
  }));

  await expect(recoverAccount('RM1-TEST')).resolves.toEqual({
    verified: true,
    recoveryCode: null,
    credentialStored: true,
  });
  expect(requests.map(({ path }) => path)).toEqual([
    '/api/recovery/options',
    '/api/auth/login/options',
    '/api/auth/login/verify',
  ]);
  expect(requests[1].body).toEqual({ credentialIds: [credentialId] });
  expect(startAuthentication).toHaveBeenCalledOnce();
});

test('recovery makes the recovered passkey the only credential used by the next normal login', async () => {
  const oldCredential = 'credential_old_1234567890';
  const otherCredential = 'credential_other_12345678';
  const recoveredCredential = 'credential_recovered_1234';
  localStorage.setItem('remoe_credential_ids', JSON.stringify([oldCredential, otherCredential]));
  localStorage.setItem('remoe_active_credential_id', otherCredential);
  startRegistration.mockResolvedValueOnce({ id: recoveredCredential, response: {} });
  startAuthentication.mockResolvedValueOnce({ id: recoveredCredential, response: {} });

  const requests = [];
  vi.stubGlobal('fetch', vi.fn(async (path, init) => {
    requests.push({ path, body: init?.body ? JSON.parse(init.body) : null });
    if (path === '/api/recovery/options') {
      return Response.json({
        challenge: 'registration-challenge',
        rp: { id: 'example.com', name: 'remoe' },
        user: { id: 'user-id', name: 'user', displayName: 'user' },
        pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
        excludeCredentials: [],
      });
    }
    if (path === '/api/auth/register/verify') {
      return Response.json({ verified: true, recoveryCode: 'RM1-NEXT' });
    }
    if (path === '/api/auth/logout') return Response.json({ ok: true });
    if (path === '/api/auth/login/options') {
      return Response.json({ challenge: 'login-challenge', rpId: 'example.com' });
    }
    if (path === '/api/auth/login/verify') return Response.json({ verified: true });
    throw new Error(`Unexpected request: ${path}`);
  }));

  await expect(recoverAccount('RM1-TEST')).resolves.toMatchObject({ credentialStored: true });
  await logout();
  await login();

  const loginOptions = requests.find(({ path }) => path === '/api/auth/login/options');
  expect(loginOptions.body).toEqual({ credentialIds: [recoveredCredential] });
  expect(JSON.parse(localStorage.getItem('remoe_credential_ids'))).toEqual([
    oldCredential, otherCredential, recoveredCredential,
  ]);
  expect(localStorage.getItem('remoe_active_credential_id')).toBe(recoveredCredential);
});

test('other-passkey login deliberately offers every locally known credential', async () => {
  const first = 'credential_first_12345678';
  const second = 'credential_second_1234567';
  localStorage.setItem('remoe_credential_ids', JSON.stringify([first, second]));
  localStorage.setItem('remoe_active_credential_id', second);
  startAuthentication.mockResolvedValueOnce({ id: first, response: {} });

  const requests = [];
  vi.stubGlobal('fetch', vi.fn(async (path, init) => {
    requests.push({ path, body: init?.body ? JSON.parse(init.body) : null });
    if (path === '/api/auth/login/options') {
      return Response.json({ challenge: 'login-challenge', rpId: 'example.com' });
    }
    if (path === '/api/auth/login/verify') return Response.json({ verified: true });
    throw new Error(`Unexpected request: ${path}`);
  }));

  await loginWithOtherPasskey();
  expect(requests[0].body).toEqual({ credentialIds: [first, second] });
  expect(localStorage.getItem('remoe_active_credential_id')).toBe(first);
});
