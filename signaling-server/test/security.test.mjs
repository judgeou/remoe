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
