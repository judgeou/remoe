<script setup lang="ts">
import { ref } from 'vue';
import type { HostSummary, PasskeySummary } from '../api';

defineProps<{
  hosts: HostSummary[];
  passkeys: PasskeySummary[];
  busy: boolean;
  error: string;
}>();

const emit = defineEmits<{
  connect: [id: string];
  refresh: [];
  logout: [];
  pair: [code: string, name: string];
  addPasskey: [];
  rotateRecovery: [];
  rename: [id: string, name: string];
  remove: [id: string];
}>();

const fps = defineModel<number>('fps', { required: true });
const bitrate = defineModel<number>('bitrate', { required: true });
const scale = defineModel<number>('scale', { required: true });

const pairingCode = ref('');
const hostName = ref('');

function relativeTime(timestamp: number | null) {
  if (!timestamp) return '从未上线';
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
  if (seconds < 60) return '刚刚在线';
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前`;
  return `${Math.floor(seconds / 86400)} 天前`;
}

function editName(host: HostSummary) {
  const value = window.prompt('设备名称', host.name)?.trim();
  if (value && value !== host.name) emit('rename', host.id, value);
}

function removeHost(host: HostSummary) {
  if (window.confirm(`删除“${host.name}”？之后可以在 Host 本机重新配对。`)) emit('remove', host.id);
}
</script>

<template>
  <section class="device-panel">
    <div class="dashboard-title">
      <div>
        <p class="eyebrow">REMOE · MY DEVICES</p>
        <h1>我的电脑</h1>
      </div>
      <div class="inline-actions">
        <button class="secondary small" :disabled="busy" @click="emit('refresh')">刷新</button>
        <button class="text-button" :disabled="busy" @click="emit('logout')">退出</button>
      </div>
    </div>

    <p v-if="error" class="notice error">{{ error }}</p>
    <div v-if="hosts.length" class="host-list">
      <article v-for="host in hosts" :key="host.id" class="host-card">
        <span class="presence" :class="{ online: host.online }" aria-hidden="true" />
        <div class="host-copy">
          <strong>{{ host.name }}</strong>
          <span>{{ host.online ? '在线' : relativeTime(host.lastSeenAt) }}</span>
        </div>
        <button :disabled="busy || !host.online" @click="emit('connect', host.id)">连接</button>
        <button class="icon-button" title="重命名" :disabled="busy" @click="editName(host)">✎</button>
        <button class="icon-button danger" title="删除" :disabled="busy" @click="removeHost(host)">×</button>
      </article>
    </div>
    <p v-else class="empty-state">还没有配对 Host。启动 remoe_host 后，把它打印的配对码填到下面。</p>

    <div class="settings dashboard-settings">
      <label><span>FPS</span><input v-model.number="fps" type="number" min="1" max="240"></label>
      <label><span>码率 Mbps</span><input v-model.number="bitrate" type="number" min="1" max="1000"></label>
      <label><span>缩放 %</span><input v-model.number="scale" type="number" min="10" max="100"></label>
    </div>

    <form class="pair-form" @submit.prevent="emit('pair', pairingCode, hostName)">
      <h2>添加 Host</h2>
      <div class="pair-fields">
        <label><span>配对码</span><input v-model.trim="pairingCode" placeholder="ABCD-EFGH" required></label>
        <label><span>设备名称</span><input v-model.trim="hostName" placeholder="家里电脑"></label>
        <button :disabled="busy || !pairingCode">添加</button>
      </div>
    </form>

    <details class="security-panel">
      <summary>账号与恢复</summary>
      <p>已注册 {{ passkeys.length }} 个 passkey。恢复码丢失时，仍可在 Host 本机使用 <code>--repair</code>。</p>
      <div class="inline-actions">
        <button class="secondary small" :disabled="busy" @click="emit('addPasskey')">添加 passkey</button>
        <button class="secondary small" :disabled="busy" @click="emit('rotateRecovery')">生成新恢复码</button>
      </div>
    </details>
  </section>
</template>
