<script setup lang="ts">
import QRCode from 'qrcode';
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import {
  decideAndroidBinding,
  getAndroidBinding,
  startAndroidBinding,
  type AndroidBindingStart,
  type AndroidBindingState,
} from '../api';

const started = ref<AndroidBindingStart | null>(null);
const binding = ref<AndroidBindingState | null>(null);
const qrDataUrl = ref('');
const busy = ref(false);
const error = ref('');
let pollTimer: number | null = null;

const terminal = computed(() => ['approved', 'rejected', 'expired'].includes(binding.value?.status ?? ''));

function stopPolling() {
  if (pollTimer !== null) window.clearTimeout(pollTimer);
  pollTimer = null;
}

function schedulePoll(delay = 1500) {
  stopPolling();
  if (!started.value || terminal.value || document.hidden) return;
  pollTimer = window.setTimeout(poll, delay);
}

async function poll() {
  if (!started.value || document.hidden) return;
  try {
    binding.value = await getAndroidBinding(started.value.bindingId);
    error.value = '';
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : String(reason);
  }
  schedulePoll();
}

function handleVisibilityChange() {
  if (document.hidden) stopPolling();
  else schedulePoll(0);
}

async function begin() {
  busy.value = true;
  error.value = '';
  stopPolling();
  try {
    started.value = await startAndroidBinding();
    binding.value = {
      bindingId: started.value.bindingId,
      status: 'created',
      expiresAt: started.value.expiresAt,
    };
    qrDataUrl.value = await QRCode.toDataURL(started.value.qrUri, {
      width: 280,
      margin: 2,
      errorCorrectionLevel: 'M',
      color: { dark: '#06100d', light: '#ffffff' },
    });
    schedulePoll();
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : String(reason);
  } finally {
    busy.value = false;
  }
}

async function decide(approve: boolean) {
  if (!started.value) return;
  busy.value = true;
  error.value = '';
  try {
    binding.value = await decideAndroidBinding(started.value.bindingId, approve);
    stopPolling();
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : String(reason);
    await poll();
  } finally {
    busy.value = false;
  }
}

onMounted(() => document.addEventListener('visibilitychange', handleVisibilityChange));
onBeforeUnmount(() => {
  stopPolling();
  document.removeEventListener('visibilitychange', handleVisibilityChange);
});
</script>

<template>
  <section class="android-bind-panel">
    <div class="android-bind-heading">
      <div>
        <p class="eyebrow">ANDROID APP</p>
        <h2>绑定 Android 手机</h2>
        <p class="help">二维码两分钟内有效，不包含账号 ID 或登录凭据。</p>
      </div>
      <button v-if="!started || terminal" class="secondary" :disabled="busy" @click="begin">
        {{ started ? '重新生成二维码' : '绑定手机' }}
      </button>
    </div>

    <p v-if="error" class="notice error">{{ error }}</p>
    <div v-if="started && binding" class="android-bind-content">
      <div v-if="binding.status === 'created'" class="qr-card">
        <img :src="qrDataUrl" alt="用于绑定 remoe Android App 的二维码">
        <div>
          <strong>用 remoe App 扫描</strong>
          <p>扫码后，本页会显示手机名称和双方一致的核对码。</p>
        </div>
      </div>

      <div v-else-if="binding.status === 'claimed'" class="bind-confirmation">
        <div>
          <span>正在绑定</span>
          <strong>{{ binding.deviceName }}</strong>
          <small>{{ binding.deviceModel }}</small>
        </div>
        <div class="comparison-code">
          <span>请确认手机显示相同核对码</span>
          <code>{{ binding.comparisonCode }}</code>
        </div>
        <div class="inline-actions">
          <button :disabled="busy" @click="decide(true)">核对一致，允许</button>
          <button class="secondary" :disabled="busy" @click="decide(false)">拒绝</button>
        </div>
      </div>

      <p v-else-if="binding.status === 'approved'" class="notice bind-success">
        已批准这台手机。请返回 App 完成下一步安全设置。
      </p>
      <p v-else-if="binding.status === 'rejected'" class="notice">已拒绝本次绑定。</p>
      <p v-else-if="binding.status === 'expired'" class="notice">二维码已过期，请重新生成。</p>
    </div>
  </section>
</template>
