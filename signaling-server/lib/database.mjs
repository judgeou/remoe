import Database from 'better-sqlite3';

export function openDatabase(filename) {
  const database = new Database(filename);
  database.pragma('journal_mode = WAL');
  database.pragma('foreign_keys = ON');
  database.pragma('busy_timeout = 5000');
  database.exec(`
    CREATE TABLE IF NOT EXISTS schema_migrations (
      version INTEGER PRIMARY KEY,
      applied_at INTEGER NOT NULL
    );
  `);

  const version = database.prepare('SELECT COALESCE(MAX(version), 0) AS version FROM schema_migrations').get().version;
  if (version < 1) {
    database.transaction(() => {
      database.exec(`
        CREATE TABLE users (
          id TEXT PRIMARY KEY,
          created_at INTEGER NOT NULL
        );

        CREATE TABLE passkeys (
          credential_id TEXT PRIMARY KEY,
          user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          public_key BLOB NOT NULL,
          counter INTEGER NOT NULL DEFAULT 0,
          transports TEXT NOT NULL DEFAULT '[]',
          backup_eligible INTEGER NOT NULL DEFAULT 0,
          backup_state INTEGER NOT NULL DEFAULT 0,
          created_at INTEGER NOT NULL,
          last_used_at INTEGER
        );
        CREATE INDEX passkeys_user_id ON passkeys(user_id);

        CREATE TABLE recovery_codes (
          lookup TEXT PRIMARY KEY,
          user_id TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
          secret_hash TEXT NOT NULL,
          created_at INTEGER NOT NULL
        );

        CREATE TABLE hosts (
          id TEXT PRIMARY KEY,
          user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          name TEXT NOT NULL,
          device_token_hash TEXT NOT NULL,
          created_at INTEGER NOT NULL,
          last_seen_at INTEGER
        );
        CREATE INDEX hosts_user_id ON hosts(user_id);

        CREATE TABLE web_sessions (
          id_hash TEXT PRIMARY KEY,
          user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          created_at INTEGER NOT NULL,
          expires_at INTEGER NOT NULL
        );
        CREATE INDEX web_sessions_expiry ON web_sessions(expires_at);
      `);
      database.prepare('INSERT INTO schema_migrations(version, applied_at) VALUES(1, ?)').run(Date.now());
    })();
  }
  if (version < 2) {
    database.transaction(() => {
      database.exec(`
        CREATE TABLE native_sessions (
          refresh_hash TEXT PRIMARY KEY,
          user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          client_name TEXT NOT NULL,
          created_at INTEGER NOT NULL,
          last_used_at INTEGER NOT NULL,
          expires_at INTEGER NOT NULL
        );
        CREATE INDEX native_sessions_user_id ON native_sessions(user_id);
        CREATE INDEX native_sessions_expiry ON native_sessions(expires_at);
      `);
      database.prepare('INSERT INTO schema_migrations(version, applied_at) VALUES(2, ?)').run(Date.now());
    })();
  }
  if (version < 3) {
    database.transaction(() => {
      database.exec(`
        CREATE TABLE android_bindings (
          id TEXT PRIMARY KEY,
          user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          qr_token_hash TEXT NOT NULL UNIQUE,
          client_secret_hash TEXT UNIQUE,
          state TEXT NOT NULL CHECK(state IN
            ('CREATED', 'CLAIMED', 'APPROVED', 'PASSKEY_CREATED', 'COMPLETED',
             'REJECTED', 'EXPIRED')),
          device_name TEXT,
          device_model TEXT,
          comparison_code TEXT,
          created_at INTEGER NOT NULL,
          expires_at INTEGER NOT NULL,
          claimed_at INTEGER,
          decided_at INTEGER
        );
        CREATE INDEX android_bindings_user_id ON android_bindings(user_id, created_at);
        CREATE INDEX android_bindings_expiry ON android_bindings(expires_at);
      `);
      database.prepare('INSERT INTO schema_migrations(version, applied_at) VALUES(3, ?)').run(Date.now());
    })();
  }
  return database;
}

export function createStore(database) {
  const statements = {
    insertUser: database.prepare('INSERT INTO users(id, created_at) VALUES(?, ?)'),
    insertPasskey: database.prepare(`
      INSERT INTO passkeys(credential_id, user_id, public_key, counter, transports,
                           backup_eligible, backup_state, created_at)
      VALUES(@credentialId, @userId, @publicKey, @counter, @transports,
             @backupEligible, @backupState, @createdAt)
    `),
    passkeysForUser: database.prepare('SELECT * FROM passkeys WHERE user_id = ? ORDER BY created_at'),
    passkeyById: database.prepare('SELECT * FROM passkeys WHERE credential_id = ?'),
    updatePasskeyUse: database.prepare(`
      UPDATE passkeys SET counter = ?, backup_state = ?, last_used_at = ? WHERE credential_id = ?
    `),
    deletePasskey: database.prepare('DELETE FROM passkeys WHERE credential_id = ? AND user_id = ?'),
    countPasskeys: database.prepare('SELECT COUNT(*) AS count FROM passkeys WHERE user_id = ?'),
    recoveryByLookup: database.prepare('SELECT * FROM recovery_codes WHERE lookup = ?'),
    deleteRecovery: database.prepare('DELETE FROM recovery_codes WHERE user_id = ?'),
    insertRecovery: database.prepare(`
      INSERT INTO recovery_codes(lookup, user_id, secret_hash, created_at) VALUES(?, ?, ?, ?)
    `),
    insertSession: database.prepare(`
      INSERT INTO web_sessions(id_hash, user_id, created_at, expires_at) VALUES(?, ?, ?, ?)
    `),
    sessionByHash: database.prepare(`
      SELECT web_sessions.*, users.id AS valid_user_id
      FROM web_sessions JOIN users ON users.id = web_sessions.user_id
      WHERE id_hash = ? AND expires_at > ?
    `),
    deleteSession: database.prepare('DELETE FROM web_sessions WHERE id_hash = ?'),
    deleteExpiredSessions: database.prepare('DELETE FROM web_sessions WHERE expires_at <= ?'),
    insertNativeSession: database.prepare(`
      INSERT INTO native_sessions(refresh_hash, user_id, client_name, created_at, last_used_at, expires_at)
      VALUES(?, ?, ?, ?, ?, ?)
    `),
    nativeSessionByHash: database.prepare(`
      SELECT native_sessions.*, users.id AS valid_user_id
      FROM native_sessions JOIN users ON users.id = native_sessions.user_id
      WHERE refresh_hash = ? AND expires_at > ?
    `),
    touchNativeSession: database.prepare(`
      UPDATE native_sessions SET last_used_at = ? WHERE refresh_hash = ?
    `),
    deleteNativeSession: database.prepare('DELETE FROM native_sessions WHERE refresh_hash = ?'),
    deleteExpiredNativeSessions: database.prepare('DELETE FROM native_sessions WHERE expires_at <= ?'),
    hostsForUser: database.prepare('SELECT * FROM hosts WHERE user_id = ? ORDER BY created_at'),
    hostForUser: database.prepare('SELECT * FROM hosts WHERE id = ? AND user_id = ?'),
    hostById: database.prepare('SELECT * FROM hosts WHERE id = ?'),
    insertHost: database.prepare(`
      INSERT INTO hosts(id, user_id, name, device_token_hash, created_at, last_seen_at)
      VALUES(?, ?, ?, ?, ?, ?)
    `),
    rebindHost: database.prepare(`
      UPDATE hosts SET user_id = ?, name = ?, device_token_hash = ?, last_seen_at = ? WHERE id = ?
    `),
    touchHost: database.prepare('UPDATE hosts SET last_seen_at = ? WHERE id = ?'),
    renameHost: database.prepare('UPDATE hosts SET name = ? WHERE id = ? AND user_id = ?'),
    deleteHost: database.prepare('DELETE FROM hosts WHERE id = ? AND user_id = ?'),
    insertAndroidBinding: database.prepare(`
      INSERT INTO android_bindings(id, user_id, qr_token_hash, state, created_at, expires_at)
      VALUES(?, ?, ?, 'CREATED', ?, ?)
    `),
    expireAndroidBindings: database.prepare(`
      UPDATE android_bindings SET state = 'EXPIRED'
      WHERE expires_at <= ? AND state IN ('CREATED', 'CLAIMED', 'APPROVED')
    `),
    androidBindingByQrToken: database.prepare(
      'SELECT * FROM android_bindings WHERE qr_token_hash = ?'),
    androidBindingForUser: database.prepare(`
      SELECT * FROM android_bindings WHERE id = ? AND user_id = ?
    `),
    claimAndroidBinding: database.prepare(`
      UPDATE android_bindings
      SET client_secret_hash = ?, device_name = ?, device_model = ?, comparison_code = ?,
          state = 'CLAIMED', claimed_at = ?
      WHERE qr_token_hash = ? AND state = 'CREATED' AND expires_at > ?
    `),
    androidBindingByClient: database.prepare(`
      SELECT * FROM android_bindings WHERE id = ? AND client_secret_hash = ?
    `),
    decideAndroidBinding: database.prepare(`
      UPDATE android_bindings SET state = ?, decided_at = ?
      WHERE id = ? AND user_id = ? AND state = 'CLAIMED' AND expires_at > ?
    `),
  };

  return {
    database,
    createUserWithPasskey(userId, passkey) {
      database.transaction(() => {
        statements.insertUser.run(userId, Date.now());
        statements.insertPasskey.run(passkey);
      })();
    },
    createUserWithPasskeyAndRecovery(userId, passkey, recovery) {
      database.transaction(() => {
        statements.insertUser.run(userId, Date.now());
        statements.insertPasskey.run(passkey);
        statements.insertRecovery.run(recovery.lookup, userId, recovery.secretHash, Date.now());
      })();
    },
    addPasskey: (passkey) => statements.insertPasskey.run(passkey),
    passkeysForUser: (userId) => statements.passkeysForUser.all(userId),
    passkeyById: (id) => statements.passkeyById.get(id),
    updatePasskeyUse: (id, counter, backupState) =>
      statements.updatePasskeyUse.run(counter, backupState ? 1 : 0, Date.now(), id),
    deletePasskey(userId, id) {
      if (statements.countPasskeys.get(userId).count <= 1) return false;
      return statements.deletePasskey.run(id, userId).changes === 1;
    },
    recoveryByLookup: (lookup) => statements.recoveryByLookup.get(lookup),
    rotateRecovery(userId, recovery) {
      database.transaction(() => {
        statements.deleteRecovery.run(userId);
        statements.insertRecovery.run(recovery.lookup, userId, recovery.secretHash, Date.now());
      })();
    },
    recoverWithPasskey(userId, passkey, recovery) {
      database.transaction(() => {
        statements.insertPasskey.run(passkey);
        statements.deleteRecovery.run(userId);
        statements.insertRecovery.run(recovery.lookup, userId, recovery.secretHash, Date.now());
      })();
    },
    createSession(hash, userId, expiresAt) {
      statements.insertSession.run(hash, userId, Date.now(), expiresAt);
    },
    sessionByHash: (hash) => statements.sessionByHash.get(hash, Date.now()),
    deleteSession: (hash) => statements.deleteSession.run(hash),
    deleteExpiredSessions: () => statements.deleteExpiredSessions.run(Date.now()),
    createNativeSession(hash, userId, clientName, expiresAt) {
      const now = Date.now();
      statements.insertNativeSession.run(hash, userId, clientName, now, now, expiresAt);
    },
    nativeSessionByHash: (hash) => statements.nativeSessionByHash.get(hash, Date.now()),
    touchNativeSession: (hash) => statements.touchNativeSession.run(Date.now(), hash),
    deleteNativeSession: (hash) => statements.deleteNativeSession.run(hash),
    deleteExpiredNativeSessions: () => statements.deleteExpiredNativeSessions.run(Date.now()),
    hostsForUser: (userId) => statements.hostsForUser.all(userId),
    hostForUser: (id, userId) => statements.hostForUser.get(id, userId),
    hostById: (id) => statements.hostById.get(id),
    createHost: (id, userId, name, tokenHash) =>
      statements.insertHost.run(id, userId, name, tokenHash, Date.now(), Date.now()),
    rebindHost: (id, userId, name, tokenHash) =>
      statements.rebindHost.run(userId, name, tokenHash, Date.now(), id),
    touchHost: (id) => statements.touchHost.run(Date.now(), id),
    renameHost: (id, userId, name) => statements.renameHost.run(name, id, userId).changes === 1,
    deleteHost: (id, userId) => statements.deleteHost.run(id, userId).changes === 1,
    createAndroidBinding(id, userId, qrTokenHash, expiresAt) {
      const now = Date.now();
      statements.insertAndroidBinding.run(id, userId, qrTokenHash, now, expiresAt);
    },
    expireAndroidBindings(now = Date.now()) {
      return statements.expireAndroidBindings.run(now).changes;
    },
    androidBindingForUser(id, userId) {
      statements.expireAndroidBindings.run(Date.now());
      return statements.androidBindingForUser.get(id, userId);
    },
    claimAndroidBinding(qrTokenHash, clientSecretHash, deviceName, deviceModel, comparisonCode) {
      const now = Date.now();
      return database.transaction(() => {
        statements.expireAndroidBindings.run(now);
        const result = statements.claimAndroidBinding.run(
          clientSecretHash, deviceName, deviceModel, comparisonCode, now, qrTokenHash, now);
        if (result.changes !== 1) return null;
        return statements.androidBindingByQrToken.get(qrTokenHash);
      })();
    },
    androidBindingByClient(id, clientSecretHash) {
      statements.expireAndroidBindings.run(Date.now());
      return statements.androidBindingByClient.get(id, clientSecretHash);
    },
    decideAndroidBinding(id, userId, decision) {
      if (decision !== 'APPROVED' && decision !== 'REJECTED') return false;
      const now = Date.now();
      return database.transaction(() => {
        statements.expireAndroidBindings.run(now);
        return statements.decideAndroidBinding.run(decision, now, id, userId, now).changes === 1;
      })();
    },
  };
}
