<script setup lang="ts">
defineProps<{
  running: boolean;
  supported: boolean;
}>();

defineEmits<{
  connect: [];
  stop: [];
}>();

const invite = defineModel<string>('invite', { required: true });
const fps = defineModel<number>('fps', { required: true });
const bitrate = defineModel<number>('bitrate', { required: true });
const scale = defineModel<number>('scale', { required: true });
</script>

<template>
  <header>
    <p class="eyebrow">REMOE · PROTOCOL V7</p>
    <h1>浏览器远程连接</h1>
    <p class="intro">通过 STUN-only WebRTC DataChannel 接收 AV1 画面并控制远程键鼠。</p>
  </header>

  <form id="connect-form" @submit.prevent="$emit('connect')">
    <label class="invite-field">
      <span>Host invite URL</span>
      <input
        v-model="invite"
        type="url"
        autocomplete="off"
        spellcheck="false"
        :disabled="running"
        placeholder="wss://signal.example.com/signal#invite"
        aria-describedby="invite-help"
      >
    </label>
    <p id="invite-help" class="help">
      也可以打开 <code>https://信令域名/#invite</code>，此时留空即可连接。
    </p>
    <div class="settings">
      <label><span>FPS</span><input v-model.number="fps" type="number" min="1" max="240"></label>
      <label><span>码率 Mbps</span><input v-model.number="bitrate" type="number" min="1" max="1000"></label>
      <label><span>缩放 %</span><input v-model.number="scale" type="number" min="10" max="100"></label>
    </div>
    <div class="actions">
      <button id="connect" type="submit" :disabled="running || !supported">连接并验证</button>
      <button id="stop" type="button" class="secondary" :disabled="!running" @click="$emit('stop')">停止</button>
    </div>
  </form>
</template>
