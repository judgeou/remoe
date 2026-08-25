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
  hosts: HostSummary[];
  passkeys: PasskeySummary[];
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
};

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
    hosts: result.hosts ?? [],
    passkeys: result.passkeys ?? [],
  };
}

async function finishRegistration(optionsPath: string, body?: object) {
  const options = await request<PublicKeyCredentialCreationOptionsJSON>(optionsPath, {
    method: 'POST', body: JSON.stringify(body ?? {}),
  });
  const credential = await startRegistration({ optionsJSON: options });
  return request<{ verified: boolean; recoveryCode: string | null }>('/api/auth/register/verify', {
    method: 'POST', body: JSON.stringify(credential),
  });
}

export const createAccount = () => finishRegistration('/api/auth/register/options');
export const addPasskey = () => finishRegistration('/api/auth/register/options');
export const recoverAccount = (code: string) => finishRegistration('/api/recovery/options', { code });

export async function login() {
  const options = await request<PublicKeyCredentialRequestOptionsJSON>('/api/auth/login/options', {
    method: 'POST', body: '{}',
  });
  const credential = await startAuthentication({ optionsJSON: options });
  await request('/api/auth/login/verify', { method: 'POST', body: JSON.stringify(credential) });
}

export const logout = () => request('/api/auth/logout', { method: 'POST', body: '{}' });

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
