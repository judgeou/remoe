import { afterEach, beforeEach, expect, test, vi } from 'vitest';

vi.mock('@simplewebauthn/browser', () => ({
  startAuthentication: vi.fn(),
  startRegistration: vi.fn(),
}));

import { startAuthentication, startRegistration } from '@simplewebauthn/browser';
import { recoverAccount } from '../src/api.ts';

const credentialId = 'credential_1234567890';

beforeEach(() => {
  vi.stubGlobal('localStorage', {
    getItem: vi.fn(() => null),
    setItem: vi.fn(),
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
  });
  expect(requests.map(({ path }) => path)).toEqual([
    '/api/recovery/options',
    '/api/auth/login/options',
    '/api/auth/login/verify',
  ]);
  expect(requests[1].body).toEqual({ credentialIds: [credentialId] });
  expect(startAuthentication).toHaveBeenCalledOnce();
});
