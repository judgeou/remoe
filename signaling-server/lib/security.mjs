import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';

const base64Url = (bytes) => Buffer.from(bytes).toString('base64url');
const recoveryAlphabet = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';

export function randomToken(bytes = 32) {
  return base64Url(randomBytes(bytes));
}

export function tokenHash(value) {
  return createHash('sha256').update(value, 'utf8').digest('base64url');
}

export function hashesEqual(left, right) {
  const leftBytes = Buffer.from(left);
  const rightBytes = Buffer.from(right);
  return leftBytes.length === rightBytes.length && timingSafeEqual(leftBytes, rightBytes);
}

function randomRecoveryCharacters(length) {
  const output = [];
  while (output.length < length) {
    for (const byte of randomBytes(length)) {
      if (byte >= 224) continue;
      output.push(recoveryAlphabet[byte % recoveryAlphabet.length]);
      if (output.length === length) break;
    }
  }
  return output.join('');
}

export function createRecoveryCode() {
  const lookup = randomRecoveryCharacters(8);
  const secret = randomRecoveryCharacters(26);
  const grouped = secret.match(/.{1,4}/g).join('-');
  return {
    lookup,
    secret,
    display: `RM1-${lookup}-${grouped}`,
    secretHash: tokenHash(secret),
  };
}

export function parseRecoveryCode(value) {
  const normalized = String(value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');
  if (!normalized.startsWith('RM1') || normalized.length !== 37) return null;
  return { lookup: normalized.slice(3, 11), secret: normalized.slice(11) };
}

export function createPairingCode() {
  const raw = randomRecoveryCharacters(8);
  return `${raw.slice(0, 4)}-${raw.slice(4)}`;
}
