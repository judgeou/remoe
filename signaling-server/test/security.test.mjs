import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createStore, openDatabase } from '../lib/database.mjs';
import {
  createPairingCode,
  createRecoveryCode,
  hashesEqual,
  parseRecoveryCode,
  tokenHash,
} from '../lib/security.mjs';

test('recovery codes round-trip after friendly formatting changes', () => {
  const recovery = createRecoveryCode();
  const parsed = parseRecoveryCode(recovery.display.toLowerCase().replaceAll('-', ' '));
  assert.equal(parsed.lookup, recovery.lookup);
  assert.equal(parsed.secret, recovery.secret);
  assert.equal(hashesEqual(recovery.secretHash, tokenHash(parsed.secret)), true);
});

test('recovery parser rejects malformed and truncated codes', () => {
  assert.equal(parseRecoveryCode(''), null);
  assert.equal(parseRecoveryCode('RM1-AAAA-BBBB'), null);
  assert.equal(parseRecoveryCode(`XX1-${'A'.repeat(34)}`), null);
});

test('pairing codes are short, unambiguous, and formatted', () => {
  const code = createPairingCode();
  assert.match(code, /^[23456789ABCDEFGHJKLMNPQRSTUVWXYZ]{4}-[23456789ABCDEFGHJKLMNPQRSTUVWXYZ]{4}$/);
});

test('recovery adds a passkey and rotates the code in one transaction', () => {
  const database = openDatabase(':memory:');
  const store = createStore(database);
  const firstRecovery = createRecoveryCode();
  const passkey = (credentialId) => ({
    credentialId,
    userId: 'user_1234567890123456',
    publicKey: Buffer.from([1, 2, 3]),
    counter: 0,
    transports: '[]',
    backupEligible: 1,
    backupState: 1,
    createdAt: Date.now(),
  });
  store.createUserWithPasskeyAndRecovery(
    'user_1234567890123456', passkey('credential_one'), firstRecovery);
  const nextRecovery = createRecoveryCode();
  store.recoverWithPasskey(
    'user_1234567890123456', passkey('credential_two'), nextRecovery);

  assert.equal(store.recoveryByLookup(firstRecovery.lookup), undefined);
  assert.equal(store.recoveryByLookup(nextRecovery.lookup).secret_hash, nextRecovery.secretHash);
  assert.equal(store.passkeysForUser('user_1234567890123456').length, 2);
  database.close();
});

test('native client refresh sessions expire and can be revoked', () => {
  const database = openDatabase(':memory:');
  const store = createStore(database);
  const userId = 'native_user_123456789';
  database.prepare('INSERT INTO users(id, created_at) VALUES(?, ?)').run(userId, Date.now());
  store.createNativeSession('refresh_hash', userId, 'Test client', Date.now() + 60_000);
  assert.equal(store.nativeSessionByHash('refresh_hash').client_name, 'Test client');
  store.deleteNativeSession('refresh_hash');
  assert.equal(store.nativeSessionByHash('refresh_hash'), undefined);
  database.close();
});

test('Android registration completes binding, passkey, and refresh session atomically', () => {
  const database = openDatabase(':memory:');
  const store = createStore(database);
  const userId = 'android_user_123456789';
  const bindingId = 'android_binding_123456789';
  database.prepare('INSERT INTO users(id, created_at) VALUES(?, ?)').run(userId, Date.now());
  store.createAndroidBinding(bindingId, userId, 'qr_hash', Date.now() + 60_000);
  assert.ok(store.claimAndroidBinding(
    'qr_hash', 'client_hash', 'Test phone', 'Test model', 'ABCD-EFGH'));
  assert.equal(store.decideAndroidBinding(bindingId, userId, 'APPROVED'), true);
  const passkey = {
    credentialId: 'android_credential_123456789', userId,
    publicKey: Buffer.from([4, 5, 6]), counter: 0, transports: '[]',
    backupEligible: 1, backupState: 1, createdAt: Date.now(),
  };
  assert.ok(store.completeAndroidRegistration(
    bindingId, 'client_hash', passkey, 'android_refresh_hash', Date.now() + 60_000));
  assert.equal(store.androidBindingByClient(bindingId, 'client_hash').state, 'COMPLETED');
  assert.equal(store.passkeyById(passkey.credentialId).user_id, userId);
  assert.equal(store.nativeSessionByHash('android_refresh_hash').user_id, userId);
  assert.equal(store.completeAndroidRegistration(
    bindingId, 'client_hash', passkey, 'another_hash', Date.now() + 60_000), null);
  database.close();
});
