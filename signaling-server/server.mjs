import http from 'node:http';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import {
  generateAuthenticationOptions,
  generateRegistrationOptions,
  verifyAuthenticationResponse,
  verifyRegistrationResponse,
} from '@simplewebauthn/server';
import { WebSocket, WebSocketServer } from 'ws';
import { createStore, openDatabase } from './lib/database.mjs';
import {
  createPairingCode,
  createRecoveryCode,
  hashesEqual,
  parseRecoveryCode,
  randomToken,
  tokenHash,
} from './lib/security.mjs';

const listenHost = process.env.REMOE_SIGNAL_HOST ?? '127.0.0.1';
const listenPort = Number.parseInt(process.env.REMOE_SIGNAL_PORT ?? '8080', 10);
const databasePath = process.env.REMOE_DATABASE_PATH ?? './data/remoe.db';
const rpId = process.env.REMOE_RP_ID ?? 'localhost';
const expectedOrigin = process.env.REMOE_ORIGIN ?? `http://localhost:${listenPort}`;
const secureCookies = expectedOrigin.startsWith('https://');
const maxPendingBytes = 2 * 1024 * 1024;
const maxTotalPendingBytes = 32 * 1024 * 1024;
const maxSessions = 1024;
const sessionIdleTimeoutMs = 2 * 60 * 1000;
const webSessionLifetimeMs = 30 * 24 * 60 * 60 * 1000;
const ceremonyLifetimeMs = 5 * 60 * 1000;
const pairingLifetimeMs = 10 * 60 * 1000;
const deviceAuthorizationLifetimeMs = 10 * 60 * 1000;
const androidBindingLifetimeMs = 2 * 60 * 1000;
const nativeAccessLifetimeMs = 15 * 60 * 1000;
const nativeRefreshLifetimeMs = 90 * 24 * 60 * 60 * 1000;
const configuredAndroidOrigins = (process.env.REMOE_ANDROID_ORIGINS ?? '').split(',')
  .map((origin) => origin.trim()).filter(Boolean);
const androidOrigins = configuredAndroidOrigins.length
  ? configuredAndroidOrigins
  : process.env.NODE_ENV === 'production' ? [] : [
      'android:apk-key-hash:BqXC8WNqInaEZFX7pQDW0GpBppYDaUWJ9AIxOhkg6ts',
    ];
const sessionPattern = /^[A-Za-z0-9_-]{8,64}$/;
const idPattern = /^[A-Za-z0-9_-]{16,64}$/;
const sessions = new Map();
const ceremonies = new Map();
const pairings = new Map();
const deviceAuthorizations = new Map();
const deviceAuthorizationCodes = new Map();
const nativeAccessTokens = new Map();
const androidCeremonies = new Map();
const managedHosts = new Map();
let totalPendingBytes = 0;

if (!Number.isInteger(listenPort) || listenPort < 1 || listenPort > 65535) {
  throw new Error('REMOE_SIGNAL_PORT must be an integer between 1 and 65535');
}
if (process.env.NODE_ENV === 'production' &&
    (!process.env.REMOE_RP_ID || !process.env.REMOE_ORIGIN || !secureCookies)) {
  throw new Error('Production requires REMOE_RP_ID and an https REMOE_ORIGIN');
}

mkdirSync(dirname(databasePath), { recursive: true });
const database = openDatabase(databasePath);
const store = createStore(database);

function parseCookies(request) {
  const cookies = {};
  for (const part of (request.headers.cookie ?? '').split(';')) {
    const separator = part.indexOf('=');
    if (separator < 1) continue;
    cookies[part.slice(0, separator).trim()] = decodeURIComponent(part.slice(separator + 1).trim());
  }
  return cookies;
}

function cookie(name, value, { maxAge, clear = false } = {}) {
  const parts = [`${name}=${encodeURIComponent(value)}`, 'Path=/', 'HttpOnly', 'SameSite=Strict'];
  if (secureCookies) parts.push('Secure');
  if (clear) parts.push('Max-Age=0');
  else if (maxAge) parts.push(`Max-Age=${Math.floor(maxAge / 1000)}`);
  return parts.join('; ');
}

function json(response, status, body, headers = {}) {
  const bytes = Buffer.from(JSON.stringify(body));
  response.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': bytes.length,
    'cache-control': 'no-store',
    ...headers,
  });
  response.end(bytes);
}

async function readJson(request) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > 256 * 1024) throw Object.assign(new Error('Request is too large'), { status: 413 });
    chunks.push(chunk);
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}');
  } catch {
    throw Object.assign(new Error('Invalid JSON'), { status: 400 });
  }
}

function requestUser(request) {
  const raw = parseCookies(request).remoe_session;
  if (!raw) return null;
  const session = store.sessionByHash(tokenHash(raw));
  return session?.user_id ?? null;
}

function requireUser(request) {
  const userId = requestUser(request);
  if (!userId) throw Object.assign(new Error('Authentication required'), { status: 401 });
  return userId;
}

function requireSameOrigin(request) {
  if (request.headers.origin !== expectedOrigin) {
    throw Object.assign(new Error('Origin rejected'), { status: 403 });
  }
}

function bearerToken(request) {
  const match = /^Bearer ([A-Za-z0-9_-]{32,256})$/.exec(request.headers.authorization ?? '');
  return match?.[1] ?? null;
}

function requireNativeSession(request) {
  const raw = bearerToken(request);
  const hash = raw ? tokenHash(raw) : null;
  const session = hash ? nativeAccessTokens.get(hash) : null;
  if (!session || session.expiresAt <= Date.now()) {
    if (hash) nativeAccessTokens.delete(hash);
    throw Object.assign(new Error('Client authentication required'), { status: 401 });
  }
  return session;
}

function issueNativeAccess(userId, refreshHash) {
  for (const [hash, session] of nativeAccessTokens) {
    if (session.refreshHash === refreshHash) nativeAccessTokens.delete(hash);
  }
  const accessToken = randomToken();
  const expiresAt = Date.now() + nativeAccessLifetimeMs;
  nativeAccessTokens.set(tokenHash(accessToken), { userId, refreshHash, expiresAt });
  return { accessToken, expiresIn: Math.floor(nativeAccessLifetimeMs / 1000) };
}

function issueNativeSession(userId, clientNameValue) {
  const refreshToken = randomToken();
  const refreshHash = tokenHash(refreshToken);
  store.createNativeSession(
    refreshHash, userId, clientNameValue, Date.now() + nativeRefreshLifetimeMs);
  return { refreshToken, ...issueNativeAccess(userId, refreshHash) };
}

function beginAndroidCeremony(state) {
  if (androidCeremonies.size >= 1024) {
    throw Object.assign(new Error('Service is busy'), { status: 503 });
  }
  const secret = randomToken();
  androidCeremonies.set(tokenHash(secret), {
    ...state,
    expiresAt: Date.now() + ceremonyLifetimeMs,
  });
  return secret;
}

function takeAndroidCeremony(value, expectedType) {
  const secret = String(value ?? '');
  const hash = tokenHash(secret);
  const ceremony = secret ? androidCeremonies.get(hash) : null;
  if (secret) androidCeremonies.delete(hash);
  if (!ceremony || ceremony.expiresAt <= Date.now() || ceremony.type !== expectedType) {
    throw Object.assign(new Error('Android authentication ceremony expired'), { status: 400 });
  }
  return ceremony;
}

function requireAndroidOrigins() {
  if (!androidOrigins.length) {
    throw Object.assign(new Error('Android passkey origin is not configured'), { status: 503 });
  }
}

function normalizeDeviceCode(value) {
  const normalized = String(value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');
  return normalized.length === 8 ? `${normalized.slice(0, 4)}-${normalized.slice(4)}` : null;
}

function clientName(value) {
  const normalized = String(value ?? '').trim().replace(/[\u0000-\u001f\u007f]/g, '');
  return normalized.slice(0, 80) || 'remoe client';
}

function deviceLabel(value, fallback) {
  const normalized = String(value ?? '').trim().replace(/[\u0000-\u001f\u007f]/g, '');
  return normalized.slice(0, 80) || fallback;
}

function publicAndroidBinding(binding) {
  return {
    bindingId: binding.id,
    status: binding.state.toLowerCase(),
    expiresAt: binding.expires_at,
    ...(binding.device_name ? { deviceName: binding.device_name } : {}),
    ...(binding.device_model ? { deviceModel: binding.device_model } : {}),
    ...(binding.comparison_code ? { comparisonCode: binding.comparison_code } : {}),
  };
}

function createLoginSession(userId) {
  const value = randomToken();
  store.createSession(tokenHash(value), userId, Date.now() + webSessionLifetimeMs);
  return cookie('remoe_session', value);
}

function beginCeremony(state) {
  const id = randomToken(24);
  ceremonies.set(id, { ...state, expiresAt: Date.now() + ceremonyLifetimeMs });
  return cookie('remoe_ceremony', id, { maxAge: ceremonyLifetimeMs });
}

function takeCeremony(request, expectedTypes) {
  const id = parseCookies(request).remoe_ceremony;
  const ceremony = id ? ceremonies.get(id) : null;
  if (id) ceremonies.delete(id);
  if (!ceremony || ceremony.expiresAt <= Date.now() || !expectedTypes.includes(ceremony.type)) {
    throw Object.assign(new Error('Authentication ceremony expired'), { status: 400 });
  }
  return ceremony;
}

function passkeyForVerification(row) {
  return {
    id: row.credential_id,
    publicKey: new Uint8Array(row.public_key),
    counter: row.counter,
    transports: JSON.parse(row.transports),
  };
}

function passkeyRecord(userId, registrationInfo, response) {
  return {
    credentialId: registrationInfo.credential.id,
    userId,
    publicKey: Buffer.from(registrationInfo.credential.publicKey),
    counter: registrationInfo.credential.counter,
    transports: JSON.stringify(response.response.transports ?? []),
    backupEligible: registrationInfo.credentialDeviceType === 'multiDevice' ? 1 : 0,
    backupState: registrationInfo.credentialBackedUp ? 1 : 0,
    createdAt: Date.now(),
  };
}

function publicHost(row) {
  return {
    id: row.id,
    name: row.name,
    online: managedHosts.get(row.id)?.readyState === WebSocket.OPEN,
    lastSeenAt: row.last_seen_at,
  };
}

function publicPasskey(row) {
  return {
    id: row.credential_id,
    createdAt: row.created_at,
    lastUsedAt: row.last_used_at,
    backedUp: Boolean(row.backup_state),
  };
}

async function createRegistrationOptions(
  userId, existing, compatibilityMode, residentKey = 'preferred') {
  const userName = `remoe-${userId.slice(0, 8)}`;
  const options = await generateRegistrationOptions({
    rpName: 'remoe',
    rpID: rpId,
    userID: new TextEncoder().encode(userId),
    userName,
    userDisplayName: userName,
    attestationType: 'none',
    excludeCredentials: existing.map((row) => ({
      id: row.credential_id,
      transports: JSON.parse(row.transports),
    })),
    authenticatorSelection: compatibilityMode ? {
      authenticatorAttachment: 'platform',
      requireResidentKey: false,
      userVerification: 'required',
    } : {
      authenticatorAttachment: 'platform',
      residentKey,
      userVerification: 'required',
    },
  });
  if (compatibilityMode) {
    delete options.authenticatorSelection.residentKey;
    options.authenticatorSelection.requireResidentKey = false;
  }
  return options;
}

function localCredentialIds(body) {
  if (!Array.isArray(body.credentialIds)) return [];
  return [...new Set(body.credentialIds)]
    .filter((id) => typeof id === 'string' && /^[A-Za-z0-9_-]{16,1400}$/.test(id))
    .slice(0, 32);
}

async function handleApi(request, response, url) {
  if (request.method === 'POST' && url.pathname === '/api/android/passkey/register/options') {
    requireAndroidOrigins();
    const body = await readJson(request);
    const bindingId = String(body.bindingId ?? '');
    const clientSecret = bearerToken(request);
    const binding = clientSecret && idPattern.test(bindingId)
      ? store.androidBindingByClient(bindingId, tokenHash(clientSecret))
      : null;
    if (!binding || binding.state !== 'APPROVED') {
      throw Object.assign(new Error('Approved Android binding required'), { status: 403 });
    }
    const options = await createRegistrationOptions(
      binding.user_id, store.passkeysForUser(binding.user_id), false, 'required');
    const ceremonyId = beginAndroidCeremony({
      type: 'register', bindingId, userId: binding.user_id,
      clientSecretHash: tokenHash(clientSecret), challenge: options.challenge,
    });
    return json(response, 200, { ceremonyId, options });
  }

  if (request.method === 'POST' && url.pathname === '/api/android/passkey/register/verify') {
    requireAndroidOrigins();
    const body = await readJson(request);
    const ceremony = takeAndroidCeremony(body.ceremonyId, 'register');
    const clientSecret = bearerToken(request);
    if (!clientSecret || !hashesEqual(ceremony.clientSecretHash, tokenHash(clientSecret)) ||
        body.bindingId !== ceremony.bindingId) {
      throw Object.assign(new Error('Android binding authentication changed'), { status: 403 });
    }
    const verification = await verifyRegistrationResponse({
      response: body.credential,
      expectedChallenge: ceremony.challenge,
      expectedOrigin: androidOrigins,
      expectedRPID: rpId,
      requireUserVerification: true,
    });
    if (!verification.verified || !verification.registrationInfo) {
      throw Object.assign(new Error('Passkey registration failed'), { status: 400 });
    }
    const record = passkeyRecord(
      ceremony.userId, verification.registrationInfo, body.credential);
    const refreshToken = randomToken();
    const refreshHash = tokenHash(refreshToken);
    const binding = store.completeAndroidRegistration(
      ceremony.bindingId, ceremony.clientSecretHash, record, refreshHash,
      Date.now() + nativeRefreshLifetimeMs,
    );
    if (!binding) {
      throw Object.assign(new Error('Android binding is no longer approved'), { status: 409 });
    }
    return json(response, 200, {
      verified: true,
      refreshToken,
      ...issueNativeAccess(binding.user_id, refreshHash),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/android/passkey/login/options') {
    requireAndroidOrigins();
    const options = await generateAuthenticationOptions({
      rpID: rpId,
      userVerification: 'required',
      allowCredentials: [],
    });
    return json(response, 200, {
      ceremonyId: beginAndroidCeremony({ type: 'login', challenge: options.challenge }),
      options,
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/android/passkey/login/verify') {
    requireAndroidOrigins();
    const body = await readJson(request);
    const ceremony = takeAndroidCeremony(body.ceremonyId, 'login');
    const credential = body.credential ?? {};
    const row = store.passkeyById(credential.id);
    if (!row) throw Object.assign(new Error('Passkey is not registered'), { status: 400 });
    const verification = await verifyAuthenticationResponse({
      response: credential,
      expectedChallenge: ceremony.challenge,
      expectedOrigin: androidOrigins,
      expectedRPID: rpId,
      credential: passkeyForVerification(row),
      requireUserVerification: true,
    });
    if (!verification.verified) {
      throw Object.assign(new Error('Passkey verification failed'), { status: 400 });
    }
    store.updatePasskeyUse(row.credential_id, verification.authenticationInfo.newCounter,
                           verification.authenticationInfo.credentialBackedUp);
    return json(response, 200, {
      verified: true,
      ...issueNativeSession(row.user_id, 'Android passkey'),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/android/bind/claim') {
    const body = await readJson(request);
    const qrToken = String(body.token ?? '');
    const clientSecret = String(body.clientSecret ?? '');
    if (!/^[A-Za-z0-9_-]{32,256}$/.test(qrToken) ||
        !/^[A-Za-z0-9_-]{32,256}$/.test(clientSecret)) {
      throw Object.assign(new Error('Binding credentials are invalid'), { status: 400 });
    }
    const binding = store.claimAndroidBinding(
      tokenHash(qrToken),
      tokenHash(clientSecret),
      deviceLabel(body.deviceName, 'Android device'),
      deviceLabel(body.deviceModel, 'Android'),
      createPairingCode(),
    );
    if (!binding) {
      throw Object.assign(new Error('Binding is invalid, expired, or already claimed'), { status: 409 });
    }
    return json(response, 200, publicAndroidBinding(binding));
  }

  if (request.method === 'POST' && url.pathname === '/api/client/device/start') {
    if (deviceAuthorizations.size >= 1024) {
      throw Object.assign(new Error('Service is busy'), { status: 503 });
    }
    const body = await readJson(request);
    const requestId = randomToken(18);
    const deviceSecret = randomToken();
    let userCode;
    do userCode = createPairingCode(); while (deviceAuthorizationCodes.has(userCode));
    const authorization = {
      requestId,
      secretHash: tokenHash(deviceSecret),
      userCode,
      clientName: clientName(body.clientName),
      expiresAt: Date.now() + deviceAuthorizationLifetimeMs,
      userId: null,
    };
    deviceAuthorizations.set(requestId, authorization);
    deviceAuthorizationCodes.set(userCode, requestId);
    return json(response, 200, {
      requestId,
      deviceSecret,
      userCode,
      verificationUrl: `${expectedOrigin}/?clientCode=${encodeURIComponent(userCode)}`,
      expiresIn: Math.floor(deviceAuthorizationLifetimeMs / 1000),
      pollInterval: 3,
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/client/device/poll') {
    const body = await readJson(request);
    const authorization = deviceAuthorizations.get(String(body.requestId ?? ''));
    if (!authorization || authorization.expiresAt <= Date.now() ||
        !hashesEqual(authorization.secretHash, tokenHash(String(body.deviceSecret ?? '')))) {
      throw Object.assign(new Error('Device authorization is invalid or expired'), { status: 400 });
    }
    if (!authorization.userId) return json(response, 202, { status: 'pending' });
    deviceAuthorizations.delete(authorization.requestId);
    deviceAuthorizationCodes.delete(authorization.userCode);
    const refreshToken = randomToken();
    const refreshHash = tokenHash(refreshToken);
    store.createNativeSession(refreshHash, authorization.userId, authorization.clientName,
                              Date.now() + nativeRefreshLifetimeMs);
    return json(response, 200, {
      status: 'authorized',
      refreshToken,
      ...issueNativeAccess(authorization.userId, refreshHash),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/client/token/refresh') {
    const body = await readJson(request);
    const refreshToken = String(body.refreshToken ?? '');
    const refreshHash = tokenHash(refreshToken);
    const session = refreshToken ? store.nativeSessionByHash(refreshHash) : null;
    if (!session) throw Object.assign(new Error('Client session is invalid or expired'), { status: 401 });
    store.touchNativeSession(refreshHash);
    return json(response, 200, issueNativeAccess(session.user_id, refreshHash));
  }

  if (request.method === 'POST' && url.pathname === '/api/client/logout') {
    const body = await readJson(request);
    const refreshHash = tokenHash(String(body.refreshToken ?? ''));
    store.deleteNativeSession(refreshHash);
    for (const [hash, session] of nativeAccessTokens) {
      if (session.refreshHash === refreshHash) nativeAccessTokens.delete(hash);
    }
    return json(response, 200, { ok: true });
  }

  if (request.method === 'GET' && url.pathname === '/api/client/hosts') {
    const session = requireNativeSession(request);
    return json(response, 200, {
      hosts: store.hostsForUser(session.userId).map(publicHost),
    });
  }

  if (request.method === 'GET' && url.pathname === '/api/client/device/authorization') {
    requireUser(request);
    const userCode = normalizeDeviceCode(url.searchParams.get('code'));
    const requestId = userCode ? deviceAuthorizationCodes.get(userCode) : null;
    const authorization = requestId ? deviceAuthorizations.get(requestId) : null;
    if (!authorization || authorization.expiresAt <= Date.now() || authorization.userId) {
      throw Object.assign(new Error('Device authorization is invalid or expired'), { status: 400 });
    }
    return json(response, 200, {
      userCode: authorization.userCode,
      clientName: authorization.clientName,
      expiresAt: authorization.expiresAt,
    });
  }

  const nativeConnectPath = url.pathname.match(
    /^\/api\/client\/hosts\/([A-Za-z0-9_-]+)\/connect$/);
  if (request.method === 'POST' && nativeConnectPath) {
    const nativeSession = requireNativeSession(request);
    const host = store.hostForUser(nativeConnectPath[1], nativeSession.userId);
    const socket = host ? managedHosts.get(host.id) : null;
    if (!host) throw Object.assign(new Error('Host not found'), { status: 404 });
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      throw Object.assign(new Error('Host is offline'), { status: 409 });
    }
    if (socket.managedSession) {
      throw Object.assign(new Error('Host is already in use'), { status: 409 });
    }
    const sessionId = randomToken(16);
    const signalingSession = createSession(sessionId, socket);
    if (!signalingSession) throw Object.assign(new Error('Service is busy'), { status: 503 });
    signalingSession.managedHostId = host.id;
    socket.managedSession = signalingSession;
    return json(response, 200, {
      invite: `${expectedOrigin.replace(/^http/, 'ws')}/signal#${sessionId}`,
    });
  }

  if (request.method === 'GET' && url.pathname === '/api/account') {
    const sessionToken = parseCookies(request).remoe_session;
    const userId = requestUser(request);
    if (!userId) return json(response, 200, { authenticated: false });
    return json(response, 200, {
      authenticated: true,
      accountId: userId,
      hosts: store.hostsForUser(userId).map(publicHost),
      passkeys: store.passkeysForUser(userId).map(publicPasskey),
    }, {
      'set-cookie': cookie('remoe_session', sessionToken),
    });
  }

  if (request.method === 'GET' && url.pathname === '/api/android/bind/status') {
    const bindingId = url.searchParams.get('id') ?? '';
    if (!idPattern.test(bindingId)) {
      throw Object.assign(new Error('Binding id is invalid'), { status: 400 });
    }
    const userId = requestUser(request);
    const clientSecret = bearerToken(request);
    const binding = userId
      ? store.androidBindingForUser(bindingId, userId)
      : clientSecret
        ? store.androidBindingByClient(bindingId, tokenHash(clientSecret))
        : null;
    if (!binding) throw Object.assign(new Error('Binding not found'), { status: 404 });
    return json(response, 200, publicAndroidBinding(binding));
  }

  if (request.method === 'POST' || request.method === 'PATCH' || request.method === 'DELETE') {
    requireSameOrigin(request);
  }

  if (request.method === 'POST' && url.pathname === '/api/android/bind/start') {
    const userId = requireUser(request);
    const bindingId = randomToken(18);
    const qrToken = randomToken();
    const expiresAt = Date.now() + androidBindingLifetimeMs;
    store.createAndroidBinding(bindingId, userId, tokenHash(qrToken), expiresAt);
    const qrUri = `remoe://bind?v=1&server=${encodeURIComponent(expectedOrigin)}&token=${qrToken}`;
    return json(response, 200, { bindingId, qrUri, expiresAt });
  }

  if (request.method === 'POST' &&
      (url.pathname === '/api/android/bind/approve' ||
       url.pathname === '/api/android/bind/reject')) {
    const userId = requireUser(request);
    const body = await readJson(request);
    const bindingId = String(body.bindingId ?? '');
    if (!idPattern.test(bindingId)) {
      throw Object.assign(new Error('Binding id is invalid'), { status: 400 });
    }
    const decision = url.pathname.endsWith('/approve') ? 'APPROVED' : 'REJECTED';
    if (!store.decideAndroidBinding(bindingId, userId, decision)) {
      throw Object.assign(new Error('Binding cannot be changed in its current state'), { status: 409 });
    }
    const binding = store.androidBindingForUser(bindingId, userId);
    return json(response, 200, publicAndroidBinding(binding));
  }

  if (request.method === 'POST' && url.pathname === '/api/client/device/authorize') {
    const userId = requireUser(request);
    const body = await readJson(request);
    const userCode = normalizeDeviceCode(body.code);
    const requestId = userCode ? deviceAuthorizationCodes.get(userCode) : null;
    const authorization = requestId ? deviceAuthorizations.get(requestId) : null;
    if (!authorization || authorization.expiresAt <= Date.now()) {
      throw Object.assign(new Error('Device authorization is invalid or expired'), { status: 400 });
    }
    if (authorization.userId) {
      throw Object.assign(new Error('Device authorization has already been approved'), { status: 409 });
    }
    authorization.userId = userId;
    return json(response, 200, { authorized: true, clientName: authorization.clientName });
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/register/options') {
    const body = await readJson(request);
    const currentUser = requestUser(request);
    const userId = currentUser ?? randomToken(18);
    const existing = currentUser ? store.passkeysForUser(userId) : [];
    const compatibilityMode = body.compatibilityMode === true;
    const options = await createRegistrationOptions(userId, existing, compatibilityMode);
    return json(response, 200, options, {
      'set-cookie': beginCeremony({
        type: currentUser ? 'add' : 'new', userId, challenge: options.challenge,
      }),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/recovery/options') {
    const body = await readJson(request);
    const parsed = parseRecoveryCode(body.code);
    const record = parsed ? store.recoveryByLookup(parsed.lookup) : null;
    if (!record || !hashesEqual(record.secret_hash, tokenHash(parsed.secret))) {
      throw Object.assign(new Error('Recovery code is invalid'), { status: 400 });
    }
    const existing = store.passkeysForUser(record.user_id);
    const options = await createRegistrationOptions(
      record.user_id, existing, body.compatibilityMode === true);
    return json(response, 200, options, {
      'set-cookie': beginCeremony({
        type: 'recover', userId: record.user_id, challenge: options.challenge,
        recoveryLookup: record.lookup, recoveryHash: record.secret_hash,
      }),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/register/verify') {
    const body = await readJson(request);
    const ceremony = takeCeremony(request, ['new', 'add', 'recover']);
    if (ceremony.type === 'add' && requestUser(request) !== ceremony.userId) {
      throw Object.assign(new Error('Authentication changed during registration'), { status: 403 });
    }
    if (ceremony.type === 'recover') {
      const current = store.recoveryByLookup(ceremony.recoveryLookup);
      if (!current || !hashesEqual(current.secret_hash, ceremony.recoveryHash)) {
        throw Object.assign(new Error('Recovery code has already been used'), { status: 400 });
      }
    }
    const verification = await verifyRegistrationResponse({
      response: body,
      expectedChallenge: ceremony.challenge,
      expectedOrigin,
      expectedRPID: rpId,
      requireUserVerification: true,
    });
    if (!verification.verified || !verification.registrationInfo) {
      throw Object.assign(new Error('Passkey registration failed'), { status: 400 });
    }
    const record = passkeyRecord(ceremony.userId, verification.registrationInfo, body);
    const recovery = ceremony.type === 'add' ? null : createRecoveryCode();
    if (ceremony.type === 'new') {
      store.createUserWithPasskeyAndRecovery(ceremony.userId, record, recovery);
    } else if (ceremony.type === 'recover') {
      store.recoverWithPasskey(ceremony.userId, record, recovery);
    } else {
      store.addPasskey(record);
    }
    return json(response, 200, {
      verified: true,
      recoveryCode: recovery?.display ?? null,
    }, {
      'set-cookie': [
        createLoginSession(ceremony.userId),
        cookie('remoe_ceremony', '', { clear: true }),
      ],
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/login/options') {
    const body = await readJson(request);
    const requestedCredentialIds = localCredentialIds(body);
    const knownCredentials = requestedCredentialIds
      .map((id) => store.passkeyById(id))
      .filter(Boolean);
    if (requestedCredentialIds.length && !knownCredentials.length) {
      throw Object.assign(new Error('Passkey is not registered'), { status: 400 });
    }
    const options = await generateAuthenticationOptions({
      rpID: rpId,
      userVerification: 'required',
      allowCredentials: knownCredentials.map((row) => ({
        id: row.credential_id,
        transports: JSON.parse(row.transports),
      })),
    });
    return json(response, 200, options, {
      'set-cookie': beginCeremony({ type: 'login', challenge: options.challenge }),
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/login/verify') {
    const body = await readJson(request);
    const ceremony = takeCeremony(request, ['login']);
    const row = store.passkeyById(body.id);
    if (!row) throw Object.assign(new Error('Passkey is not registered'), { status: 400 });
    const verification = await verifyAuthenticationResponse({
      response: body,
      expectedChallenge: ceremony.challenge,
      expectedOrigin,
      expectedRPID: rpId,
      credential: passkeyForVerification(row),
      requireUserVerification: true,
    });
    if (!verification.verified) throw Object.assign(new Error('Passkey verification failed'), { status: 400 });
    store.updatePasskeyUse(row.credential_id, verification.authenticationInfo.newCounter,
                           verification.authenticationInfo.credentialBackedUp);
    return json(response, 200, { verified: true }, {
      'set-cookie': [
        createLoginSession(row.user_id),
        cookie('remoe_ceremony', '', { clear: true }),
      ],
    });
  }

  if (request.method === 'POST' && url.pathname === '/api/auth/logout') {
    const raw = parseCookies(request).remoe_session;
    if (raw) store.deleteSession(tokenHash(raw));
    return json(response, 200, { ok: true }, {
      'set-cookie': cookie('remoe_session', '', { clear: true }),
    });
  }

  const passkeyDelete = url.pathname.match(/^\/api\/passkeys\/([A-Za-z0-9_-]+)$/);
  if (request.method === 'DELETE' && passkeyDelete) {
    const userId = requireUser(request);
    if (!store.deletePasskey(userId, passkeyDelete[1])) {
      throw Object.assign(new Error('The last passkey cannot be removed'), { status: 400 });
    }
    return json(response, 200, { ok: true });
  }

  if (request.method === 'POST' && url.pathname === '/api/recovery/rotate') {
    const userId = requireUser(request);
    const recovery = createRecoveryCode();
    store.rotateRecovery(userId, recovery);
    return json(response, 200, { recoveryCode: recovery.display });
  }

  if (request.method === 'POST' && url.pathname === '/api/hosts/pair') {
    const userId = requireUser(request);
    const body = await readJson(request);
    const normalizedCode = String(body.code ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');
    const pairing = [...pairings.values()].find((item) =>
      item.normalizedCode === normalizedCode && item.expiresAt > Date.now());
    if (!pairing || pairing.claimed) {
      throw Object.assign(new Error('Pairing code is invalid or expired'), { status: 400 });
    }
    const name = String(body.name ?? '').trim().slice(0, 80) || 'Windows Host';
    const deviceId = pairing.hostId ?? randomToken(18);
    const deviceToken = randomToken();
    if (pairing.hostId && store.hostById(pairing.hostId)) {
      store.rebindHost(deviceId, userId, name, tokenHash(deviceToken));
      managedHosts.get(deviceId)?.close(4003, 'Host rebound');
    } else {
      store.createHost(deviceId, userId, name, tokenHash(deviceToken));
    }
    pairing.claimed = true;
    pairing.socket.send(`paired:${deviceId}:${deviceToken}`);
    pairing.socket.close(1000, 'Paired');
    pairings.delete(pairing.id);
    return json(response, 200, { host: publicHost(store.hostById(deviceId)) });
  }

  const hostPath = url.pathname.match(/^\/api\/hosts\/([A-Za-z0-9_-]+)$/);
  if (hostPath && request.method === 'PATCH') {
    const userId = requireUser(request);
    const body = await readJson(request);
    const name = String(body.name ?? '').trim().slice(0, 80);
    if (!name || !store.renameHost(hostPath[1], userId, name)) {
      throw Object.assign(new Error('Host not found or name is empty'), { status: 400 });
    }
    return json(response, 200, { ok: true });
  }
  if (hostPath && request.method === 'DELETE') {
    const userId = requireUser(request);
    if (!store.deleteHost(hostPath[1], userId)) {
      throw Object.assign(new Error('Host not found'), { status: 404 });
    }
    managedHosts.get(hostPath[1])?.close(4003, 'Host revoked');
    return json(response, 200, { ok: true });
  }

  const connectPath = url.pathname.match(/^\/api\/hosts\/([A-Za-z0-9_-]+)\/connect$/);
  if (connectPath && request.method === 'POST') {
    const userId = requireUser(request);
    const host = store.hostForUser(connectPath[1], userId);
    const socket = host ? managedHosts.get(host.id) : null;
    if (!host) throw Object.assign(new Error('Host not found'), { status: 404 });
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      throw Object.assign(new Error('Host is offline'), { status: 409 });
    }
    if (socket.managedSession) {
      throw Object.assign(new Error('Host is already in use'), { status: 409 });
    }
    const sessionId = randomToken(16);
    const session = createSession(sessionId, socket);
    if (!session) throw Object.assign(new Error('Service is busy'), { status: 503 });
    session.managedHostId = host.id;
    socket.managedSession = session;
    return json(response, 200, { invite: `${expectedOrigin.replace(/^http/, 'ws')}/signal#${sessionId}` });
  }

  return json(response, 404, { error: 'Not found' });
}

const server = http.createServer(async (request, response) => {
  try {
    const url = new URL(request.url, 'http://localhost');
    if (request.method === 'GET' && url.pathname === '/healthz') {
      return json(response, 200, {
        status: 'ok', sessions: sessions.size, managedHosts: managedHosts.size,
      });
    }
    if (url.pathname.startsWith('/api/')) return await handleApi(request, response, url);
    response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
    response.end('Not Found\n');
  } catch (error) {
    const status = Number.isInteger(error.status) ? error.status : 500;
    if (status === 500) console.error(error);
    json(response, status, { error: status === 500 ? 'Internal server error' : error.message });
  }
});

const websocketServer = new WebSocketServer({
  noServer: true,
  maxPayload: 1024 * 1024 + 1024,
  perMessageDeflate: false,
});

function rejectUpgrade(socket, status, reason) {
  socket.write(`HTTP/1.1 ${status} ${reason}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  socket.destroy();
}

function createSession(id, existingHost = null) {
  if (sessions.size >= maxSessions) return null;
  const session = {
    id,
    host: existingHost,
    client: null,
    pendingHost: [],
    pendingHostBytes: 0,
    pendingClient: [],
    pendingClientBytes: 0,
    lastActivity: Date.now(),
    managedHostId: null,
  };
  sessions.set(id, session);
  return session;
}

function peerRole(role) {
  return role === 'host' ? 'client' : 'host';
}

function enqueueFor(session, destinationRole, message) {
  const queueName = destinationRole === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = destinationRole === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  if (session[bytesName] + message.length > maxPendingBytes ||
      totalPendingBytes + message.length > maxTotalPendingBytes) return false;
  session[queueName].push(Buffer.from(message));
  session[bytesName] += message.length;
  totalPendingBytes += message.length;
  return true;
}

function flushPending(session, role, socket) {
  const queueName = role === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = role === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  for (const message of session[queueName]) socket.send(message, { binary: true });
  totalPendingBytes -= session[bytesName];
  session[queueName] = [];
  session[bytesName] = 0;
}

function discardPendingFor(session, destinationRole) {
  const queueName = destinationRole === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = destinationRole === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  totalPendingBytes -= session[bytesName];
  session[queueName] = [];
  session[bytesName] = 0;
}

function discardPending(session) {
  discardPendingFor(session, 'host');
  discardPendingFor(session, 'client');
}

function removeSession(session) {
  discardPending(session);
  sessions.delete(session.id);
  if (session.managedHostId && session.host?.managedSession === session) {
    session.host.managedSession = null;
  }
}

function offeredProtocol(request) {
  return (request.headers['sec-websocket-protocol'] ?? '').split(',')[0].trim();
}

function parseHostProtocol(protocol) {
  const parts = protocol.split('.');
  if (parts[0] === 'remoe-host' && parts.length === 3 && idPattern.test(parts[1])) {
    return { type: 'host', hostId: parts[1], token: parts[2] };
  }
  if (parts[0] === 'remoe-pair' && parts[1] === 'new' && parts.length === 2) {
    return { type: 'pair', hostId: null };
  }
  if (parts[0] === 'remoe-pair' && parts.length === 3 && idPattern.test(parts[1])) {
    return { type: 'pair', hostId: parts[1], token: parts[2] };
  }
  return null;
}

function validateHostAuth(hostId, token) {
  const host = store.hostById(hostId);
  return host && hashesEqual(host.device_token_hash, tokenHash(token)) ? host : null;
}

server.on('upgrade', (request, socket, head) => {
  let url;
  try {
    url = new URL(request.url, 'http://localhost');
  } catch {
    return rejectUpgrade(socket, 400, 'Bad Request');
  }

  if (url.pathname === '/host') {
    const auth = parseHostProtocol(offeredProtocol(request));
    if (!auth || (auth.hostId && !validateHostAuth(auth.hostId, auth.token))) {
      return rejectUpgrade(socket, 401, 'Unauthorized');
    }
    return websocketServer.handleUpgrade(request, socket, head, (websocket) => {
      websocketServer.emit('connection', websocket, request, auth);
    });
  }

  if (url.pathname !== '/signal') return rejectUpgrade(socket, 404, 'Not Found');
  const id = url.searchParams.get('session') ?? '';
  const role = url.searchParams.get('role') ?? '';
  if (!sessionPattern.test(id) || (role !== 'host' && role !== 'client')) {
    return rejectUpgrade(socket, 400, 'Bad Request');
  }
  let session = sessions.get(id);
  let rejection = null;
  if (role === 'client' && (!session || session.host?.readyState !== WebSocket.OPEN)) {
    rejection = 'invite-not-found';
  }
  if (!session && role === 'host') session = createSession(id);
  if (!session && !rejection) rejection = 'service-unavailable';
  if (session?.[role]) rejection = role === 'client' ? 'invite-in-use' : 'host-in-use';
  websocketServer.handleUpgrade(request, socket, head, (websocket) => {
    websocketServer.emit('connection', websocket, request, { type: 'signal', id, role, session, rejection });
  });
});

function handlePairingSocket(socket, context) {
  const id = randomToken(18);
  const code = createPairingCode();
  const pairing = {
    id,
    socket,
    hostId: context.hostId,
    code,
    normalizedCode: code.replace('-', ''),
    expiresAt: Date.now() + pairingLifetimeMs,
    claimed: false,
  };
  pairings.set(id, pairing);
  socket.isAlive = true;
  socket.send(`pairing:${code}`);
  socket.on('pong', () => { socket.isAlive = true; });
  socket.on('close', () => pairings.delete(id));
  socket.on('error', (error) => console.error('Pairing WebSocket error:', error.message));
}

function handleManagedHost(socket, context) {
  if (managedHosts.has(context.hostId)) {
    socket.send('error:host-in-use');
    socket.close(4004, 'Host already online');
    return;
  }
  socket.isAlive = true;
  socket.managedSession = null;
  socket.hostId = context.hostId;
  managedHosts.set(context.hostId, socket);
  store.touchHost(context.hostId);
  socket.send('registered');
  socket.on('pong', () => {
    socket.isAlive = true;
    store.touchHost(context.hostId);
  });
  socket.on('message', (message, isBinary) => {
    const session = socket.managedSession;
    if (!session) {
      socket.close(1008, 'No authorized client session');
      return;
    }
    relayMessage(socket, session, 'host', message, isBinary);
  });
  socket.on('close', () => {
    if (managedHosts.get(context.hostId) === socket) managedHosts.delete(context.hostId);
    if (socket.managedSession) {
      socket.managedSession.client?.close(4001, 'Host disconnected');
      removeSession(socket.managedSession);
    }
  });
  socket.on('error', (error) => console.error(`Managed Host ${context.hostId}:`, error.message));
}

function relayMessage(socket, session, role, message, isBinary) {
  session.lastActivity = Date.now();
  if (!isBinary) {
    socket.close(1003, 'Binary signaling messages required');
    return;
  }
  const destinationRole = peerRole(role);
  const destination = session[destinationRole];
  if (destination?.readyState === WebSocket.OPEN) {
    destination.send(message, { binary: true });
  } else if (!enqueueFor(session, destinationRole, message)) {
    socket.close(1009, 'Pending signaling limit exceeded');
  }
}

function handleSignalSocket(socket, context) {
  const { id, role, session, rejection } = context;
  if (rejection) {
    socket.send(`error:${rejection}`);
    socket.close(4004, 'Registration rejected');
    return;
  }
  socket.isAlive = true;
  session[role] = socket;
  session.lastActivity = Date.now();
  socket.send('registered');
  flushPending(session, role, socket);
  socket.on('pong', () => {
    socket.isAlive = true;
    session.lastActivity = Date.now();
  });
  socket.on('message', (message, isBinary) => relayMessage(socket, session, role, message, isBinary));
  socket.on('error', (error) => console.error(`WebSocket error for ${role} session ${id}:`, error.message));
  socket.on('close', () => {
    if (session[role] === socket) {
      session[role] = null;
      discardPendingFor(session, peerRole(role));
    }
    if (role === 'host') {
      session.client?.close(4001, 'Invite expired');
      removeSession(session);
    } else if (session.managedHostId) {
      session.host?.close(4001, 'Client disconnected');
      removeSession(session);
    } else if (!session.managedHostId && !session.host) {
      removeSession(session);
    }
  });
}

websocketServer.on('connection', (socket, request, context) => {
  if (context.type === 'pair') return handlePairingSocket(socket, context);
  if (context.type === 'host') return handleManagedHost(socket, context);
  return handleSignalSocket(socket, context);
});

const heartbeat = setInterval(() => {
  for (const socket of websocketServer.clients) {
    if (!socket.isAlive) {
      socket.terminate();
      continue;
    }
    socket.isAlive = false;
    socket.ping();
  }
  const now = Date.now();
  for (const [id, ceremony] of ceremonies) if (ceremony.expiresAt <= now) ceremonies.delete(id);
  for (const [id, ceremony] of androidCeremonies) {
    if (ceremony.expiresAt <= now) androidCeremonies.delete(id);
  }
  for (const [id, authorization] of deviceAuthorizations) {
    if (authorization.expiresAt > now) continue;
    deviceAuthorizations.delete(id);
    deviceAuthorizationCodes.delete(authorization.userCode);
  }
  for (const [hash, session] of nativeAccessTokens) {
    if (session.expiresAt <= now) nativeAccessTokens.delete(hash);
  }
  for (const [id, pairing] of pairings) {
    if (pairing.expiresAt > now) continue;
    pairing.socket.close(4008, 'Pairing expired');
    pairings.delete(id);
  }
  for (const session of sessions.values()) {
    if (session.lastActivity >= now - sessionIdleTimeoutMs) continue;
    session.client?.terminate();
    if (!session.managedHostId) session.host?.terminate();
    removeSession(session);
  }
  store.deleteExpiredSessions();
  store.deleteExpiredNativeSessions();
  store.expireAndroidBindings(now);
}, 30_000);
heartbeat.unref();

server.listen(listenPort, listenHost, () => {
  console.log(`remoe signaling server listening on ${listenHost}:${listenPort}`);
});

function shutdown() {
  clearInterval(heartbeat);
  for (const socket of websocketServer.clients) socket.close(1001, 'Server shutting down');
  server.close(() => {
    database.close();
    process.exit(0);
  });
  setTimeout(() => process.exit(1), 5_000).unref();
}

process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
