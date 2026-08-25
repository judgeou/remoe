<script setup lang="ts">
import { computed, ref } from 'vue';

type ResultState = 'idle' | 'running' | 'success' | 'error';

interface Variant {
  id: string;
  title: string;
  change: string;
  selection: AuthenticatorSelectionCriteria;
  algorithms: number[];
  emptyDisplayName?: boolean;
}

interface Result {
  state: ResultState;
  detail: string;
}

const variants: Variant[] = [
  {
    id: 'showcase',
    title: 'A · Showcase 原样',
    change: 'platform · requireResidentKey=false · UV preferred',
    selection: {
      authenticatorAttachment: 'platform', requireResidentKey: false, userVerification: 'preferred',
    },
    algorithms: [-7, -257],
  },
  {
    id: 'resident-preferred',
    title: 'B · 偏好可发现凭据',
    change: 'A + residentKey=preferred',
    selection: {
      authenticatorAttachment: 'platform', residentKey: 'preferred',
      requireResidentKey: false, userVerification: 'preferred',
    },
    algorithms: [-7, -257],
  },
  {
    id: 'resident-required',
    title: 'C · 强制可发现凭据',
    change: 'A + residentKey=required · requireResidentKey=true',
    selection: {
      authenticatorAttachment: 'platform', residentKey: 'required',
      requireResidentKey: true, userVerification: 'preferred',
    },
    algorithms: [-7, -257],
  },
  {
    id: 'uv-required',
    title: 'D · 强制用户验证',
    change: 'A + userVerification=required',
    selection: {
      authenticatorAttachment: 'platform', requireResidentKey: false, userVerification: 'required',
    },
    algorithms: [-7, -257],
  },
  {
    id: 'attachment-unspecified',
    title: 'E · 不指定认证器',
    change: 'A - authenticatorAttachment',
    selection: { requireResidentKey: false, userVerification: 'preferred' },
    algorithms: [-7, -257],
  },
  {
    id: 'eddsa',
    title: 'F · 加入 EdDSA',
    change: 'A + EdDSA(-8) 并放在首位',
    selection: {
      authenticatorAttachment: 'platform', requireResidentKey: false, userVerification: 'preferred',
    },
    algorithms: [-8, -7, -257],
  },
  {
    id: 'empty-display-name',
    title: 'G · 空显示名称',
    change: 'A + user.displayName=""',
    selection: {
      authenticatorAttachment: 'platform', requireResidentKey: false, userVerification: 'preferred',
    },
    algorithms: [-7, -257],
    emptyDisplayName: true,
  },
  {
    id: 'remoe-current',
    title: 'H · remoe 当前组合',
    change: 'platform · residentKey=preferred · UV required · EdDSA · 空名称',
    selection: {
      authenticatorAttachment: 'platform', residentKey: 'preferred',
      requireResidentKey: false, userVerification: 'required',
    },
    algorithms: [-8, -7, -257],
    emptyDisplayName: true,
  },
];

const results = ref<Record<string, Result>>(
  Object.fromEntries(variants.map((variant) => [variant.id, { state: 'idle', detail: '尚未测试' }])),
);
const active = ref('');
const copied = ref(false);
const supported = Boolean(window.PublicKeyCredential && navigator.credentials?.create);
const userId = crypto.getRandomValues(new Uint8Array(32));

function randomBytes(): Uint8Array<ArrayBuffer> {
  return crypto.getRandomValues(new Uint8Array(32));
}

function errorDetail(error: unknown): string {
  if (error instanceof DOMException) return `${error.name}: ${error.message}`;
  if (error instanceof Error) return `${error.name}: ${error.message}`;
  return String(error);
}

async function run(variant: Variant) {
  active.value = variant.id;
  results.value[variant.id] = { state: 'running', detail: '等待系统凭据管理器…' };
  try {
    const credential = await navigator.credentials.create({
      publicKey: {
        challenge: randomBytes(),
        rp: { name: 'remoe WebAuthn diagnostics', id: location.hostname },
        user: {
          id: userId,
          name: 'remoe-diagnostics',
          displayName: variant.emptyDisplayName ? '' : 'remoe diagnostics',
        },
        pubKeyCredParams: variant.algorithms.map((alg) => ({ alg, type: 'public-key' })),
        authenticatorSelection: variant.selection,
        extensions: { credProps: true } as AuthenticationExtensionsClientInputs,
        timeout: 60_000,
        attestation: 'none',
      },
    }) as PublicKeyCredential | null;
    if (!credential) throw new Error('浏览器没有返回凭据');
    const response = credential.response as AuthenticatorAttestationResponse;
    const transports = response.getTransports?.().join(', ') || '未报告';
    const extensions = credential.getClientExtensionResults() as
      AuthenticationExtensionsClientOutputs & { credProps?: { rk?: boolean } };
    const discoverable = extensions.credProps?.rk;
    results.value[variant.id] = {
      state: 'success',
      detail: `成功 · rk=${discoverable ?? '未报告'} · ` +
        `attachment=${credential.authenticatorAttachment ?? '未报告'} · transports=${transports}`,
    };
  } catch (error) {
    results.value[variant.id] = { state: 'error', detail: errorDetail(error) };
  } finally {
    active.value = '';
  }
}

const report = computed(() => variants.map((variant) =>
  `${variant.title}: ${results.value[variant.id].detail}`).join('\n'));

async function copyReport() {
  await navigator.clipboard.writeText([
    navigator.userAgent,
    `Host: ${location.hostname}`,
    report.value,
  ].join('\n'));
  copied.value = true;
  window.setTimeout(() => { copied.value = false; }, 1_500);
}
</script>

<template>
  <main class="diagnostics-page">
    <header class="diagnostics-header">
      <p class="eyebrow">REMOE · WEBAUTHN DIAGNOSTICS</p>
      <h1>逐项测试创建参数</h1>
      <p>
        A 是已知可用的 Showcase 参数；B–G 每次只改变一个条件；H 是 remoe 当前组合。
        请逐个点击并完成或关闭系统弹窗。
      </p>
      <aside class="diagnostics-warning">
        成功的测试可能在密码管理工具中留下名为“remoe WebAuthn diagnostics”的测试凭据，完成后可以手动删除。
      </aside>
    </header>

    <p v-if="!supported" class="error diagnostics-unsupported">当前浏览器不支持 WebAuthn。</p>

    <section class="diagnostics-grid" aria-label="WebAuthn 参数测试">
      <article v-for="variant in variants" :key="variant.id" class="diagnostic-card">
        <button type="button" :disabled="!supported || Boolean(active)" @click="run(variant)">
          {{ active === variant.id ? '测试中…' : variant.title }}
        </button>
        <code>{{ variant.change }}</code>
        <p :class="['diagnostic-result', results[variant.id].state]">
          {{ results[variant.id].detail }}
        </p>
      </article>
    </section>

    <div class="diagnostics-actions">
      <button class="secondary" type="button" @click="copyReport">
        {{ copied ? '已复制' : '复制测试结果' }}
      </button>
      <a href="/">返回 remoe</a>
    </div>
  </main>
</template>
