import { startAuthentication, startRegistration } from '@simplewebauthn/browser';
import type {
  PublicKeyCredentialCreationOptionsJSON,
  PublicKeyCredentialRequestOptionsJSON,
} from '@simplewebauthn/browser';

export interface HostSummary {
  id: string;
  name: string;
  online: boolean;
  lastSeenAt: number | null;
}

export interface PasskeySummary {
  id: string;
  createdAt: number;
  lastUsedAt: number | null;
  backedUp: boolean;
}

export interface AccountState {
  authenticated: boolean;
  accountId: string | null;
  hosts: HostSummary[];
  passkeys: PasskeySummary[];
}

export interface CredentialActionResult {
  credentialStored: boolean;
}

export type AndroidBindingStatus =
  | 'created' | 'claimed' | 'approved' | 'rejected' | 'expired';

export interface AndroidBindingStart {
  bindingId: string;
  qrUri: string;
  expiresAt: number;
}

export interface AndroidBindingState {
  bindingId: string;
  status: AndroidBindingStatus;
  expiresAt: number;
  deviceName?: string;
  deviceModel?: string;
  comparisonCode?: string;
}

export interface RegistrationResult extends CredentialActionResult {
  verified: boolean;
  recoveryCode: string | null;
}

const errorMessages: Record<string, string> = {
  'Authentication required': '请先使用 passkey 登录',
  'Recovery code is invalid': '恢复码无效',
  'Recovery code has already been used': '这个恢复码已经使用过，请使用最新恢复码',
  'Authentication ceremony expired': '验证已超时，请重试',
  'Host is offline': 'Host 当前离线',
  'Host is already in use': 'Host 正在被连接',
  'Host not found': '找不到这台 Host',
  'Pairing code is invalid or expired': '配对码无效或已经过期',
  'The last passkey cannot be removed': '不能删除账号最后一个 passkey',
  'Passkey is not registered': '当前 passkey 不属于这个服务，请尝试“使用其他 passkey”或账号恢复码',
  'Binding cannot be changed in its current state': '绑定状态已改变，请重新开始',
  'Binding not found': '找不到这次绑定，请重新开始',
};

const credentialIdsKey = 'remoe_credential_ids';
const activeCredentialIdKey = 'remoe_active_credential_id';
const compatibilityModeKey = 'remoe_webauthn_compatibility';
const credentialIdPattern = /^[A-Za-z0-9_-]{16,1400}$/;

function storedCredentialIds(): string[] {
  try {
    const value = JSON.parse(localStorage.getItem(credentialIdsKey) ?? '[]');
    if (!Array.isArray(value)) return [];
    return [...new Set(value)]
      .filter((id): id is string => typeof id === 'string' && credentialIdPattern.test(id))
      .slice(-32);
  } catch {
    return [];
  }
}

function activeCredentialIds(): string[] {
  try {
    const active = localStorage.getItem(activeCredentialIdKey);
    if (active && credentialIdPattern.test(active)) return [active];
  } catch {
    return [];
  }
  const stored = storedCredentialIds();
  return stored.length ? [stored.at(-1)!] : [];
}

function rememberCredentialId(id: string): boolean {
  if (!credentialIdPattern.test(id)) return false;
  try {
    localStorage.setItem(credentialIdsKey,
      JSON.stringify([...new Set([...storedCredentialIds(), id])].slice(-32)));
    localStorage.setItem(activeCredentialIdKey, id);
    return localStorage.getItem(activeCredentialIdKey) === id && storedCredentialIds().includes(id);
  } catch {
    // Private browsing or storage policy may make localStorage unavailable.
    return false;
  }
}

function prefersCompatibilityMode(): boolean {
  try {
    return localStorage.getItem(compatibilityModeKey) === 'true';
  } catch {
    return false;
  }
}

function rememberCompatibilityMode() {
  try {
    localStorage.setItem(compatibilityModeKey, 'true');
  } catch {
    // The current attempt can still continue without remembering the preference.
  }
}

function isCredentialManagerFailure(error: unknown): boolean {
  if (!(error instanceof Error)) return false;
  const cause = error.cause instanceof Error ? error.cause : null;
  return [error, cause].some((candidate) => candidate?.name === 'NotReadableError' &&
    candidate.message.toLowerCase().includes('credential manager'));
}

function isPreviouslyRegistered(error: unknown): boolean {
  if (!(error instanceof Error)) return false;
  const candidate = error as Error & { code?: string };
  return candidate.code === 'ERROR_AUTHENTICATOR_PREVIOUSLY_REGISTERED' ||
    candidate.name === 'InvalidStateError' ||
    (candidate.cause instanceof Error && candidate.cause.name === 'InvalidStateError');
}

function readableRegistrationError(error: unknown): Error {
  if (isPreviouslyRegistered(error)) {
    return new Error('这台设备已有该账号的 passkey，请直接使用 passkey 登录');
  }
  return error instanceof Error ? error : new Error(String(error));
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, {
    ...init,
    credentials: 'same-origin',
    headers: init.body ? { 'content-type': 'application/json', ...init.headers } : init.headers,
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message = body.error || `请求失败 (${response.status})`;
    throw new Error(errorMessages[message] ?? message);
  }
  return body as T;
}

export async function getAccount(): Promise<AccountState> {
  const result = await request<Partial<AccountState>>('/api/account');
  return {
    authenticated: Boolean(result.authenticated),
    accountId: result.accountId ?? null,
    hosts: result.hosts ?? [],
    passkeys: result.passkeys ?? [],
  };
}

async function authenticate(credentialIds: string[]): Promise<CredentialActionResult> {
  const options = await request<PublicKeyCredentialRequestOptionsJSON>('/api/auth/login/options', {
    method: 'POST', body: JSON.stringify({ credentialIds }),
  });
  const credential = await startAuthentication({ optionsJSON: options });
  await request('/api/auth/login/verify', { method: 'POST', body: JSON.stringify(credential) });
  return { credentialStored: rememberCredentialId(credential.id) };
}

async function finishRegistration(
  optionsPath: string, body?: object, loginOnDuplicate = false,
): Promise<RegistrationResult> {
  const optionsFor = (compatibilityMode: boolean) =>
    request<PublicKeyCredentialCreationOptionsJSON>(optionsPath, {
      method: 'POST', body: JSON.stringify({ ...body, compatibilityMode }),
    });
  let compatibilityMode = prefersCompatibilityMode();
  let options = await optionsFor(compatibilityMode);
  let credential;
  try {
    try {
      credential = await startRegistration({ optionsJSON: options });
    } catch (error) {
      if (compatibilityMode || !isCredentialManagerFailure(error)) throw error;
      compatibilityMode = true;
      rememberCompatibilityMode();
      options = await optionsFor(true);
      credential = await startRegistration({ optionsJSON: options });
    }
  } catch (error) {
    if (!loginOnDuplicate || !isPreviouslyRegistered(error)) {
      throw readableRegistrationError(error);
    }
    const authentication = await authenticate((options.excludeCredentials ?? []).map(({ id }) => id));
    return { verified: true, recoveryCode: null, ...authentication };
  }
  const result = await request<{ verified: boolean; recoveryCode: string | null }>(
    '/api/auth/register/verify', {
    method: 'POST', body: JSON.stringify(credential),
  });
  const credentialStored = rememberCredentialId(credential.id);
  if (compatibilityMode) rememberCompatibilityMode();
  return { ...result, credentialStored };
}

export const createAccount = () => finishRegistration('/api/auth/register/options');
export const addPasskey = () => finishRegistration('/api/auth/register/options');
export const recoverAccount = (code: string) =>
  finishRegistration('/api/recovery/options', { code }, true);

export const login = () => authenticate(activeCredentialIds());

export const loginWithOtherPasskey = () => authenticate(storedCredentialIds());

export const logout = () => request('/api/auth/logout', { method: 'POST', body: '{}' });

export const authorizeNativeClient = (code: string) =>
  request<{ authorized: boolean; clientName: string }>('/api/client/device/authorize', {
    method: 'POST', body: JSON.stringify({ code }),
  });

export const getNativeClientAuthorization = (code: string) =>
  request<{ userCode: string; clientName: string; expiresAt: number }>(
    `/api/client/device/authorization?code=${encodeURIComponent(code)}`);

export const pairHost = (code: string, name: string) =>
  request('/api/hosts/pair', { method: 'POST', body: JSON.stringify({ code, name }) });

export async function connectHost(id: string): Promise<string> {
  const result = await request<{ invite: string }>(`/api/hosts/${encodeURIComponent(id)}/connect`, {
    method: 'POST', body: '{}',
  });
  return result.invite;
}

export async function rotateRecoveryCode(): Promise<string> {
  const result = await request<{ recoveryCode: string }>('/api/recovery/rotate', {
    method: 'POST', body: '{}',
  });
  return result.recoveryCode;
}

export const renameHost = (id: string, name: string) =>
  request(`/api/hosts/${encodeURIComponent(id)}`, {
    method: 'PATCH', body: JSON.stringify({ name }),
  });

export const deleteHost = (id: string) =>
  request(`/api/hosts/${encodeURIComponent(id)}`, { method: 'DELETE' });

export const startAndroidBinding = () =>
  request<AndroidBindingStart>('/api/android/bind/start', { method: 'POST', body: '{}' });

export const getAndroidBinding = (bindingId: string) =>
  request<AndroidBindingState>(
    `/api/android/bind/status?id=${encodeURIComponent(bindingId)}`);

export const decideAndroidBinding = (bindingId: string, approve: boolean) =>
  request<AndroidBindingState>(`/api/android/bind/${approve ? 'approve' : 'reject'}`, {
    method: 'POST', body: JSON.stringify({ bindingId }),
  });
