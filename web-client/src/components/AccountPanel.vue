<script setup lang="ts">
import { ref } from 'vue';

defineProps<{ busy: boolean; error: string }>();
const emit = defineEmits<{
  login: [];
  loginOther: [];
  create: [];
  recover: [code: string];
}>();

const recovering = ref(false);
const recoveryCode = ref('');
</script>

<template>
  <section class="account-panel">
    <p class="eyebrow">REMOE · PASSKEY</p>
    <h1>你的远程电脑</h1>
    <p class="intro">使用 Windows Hello、手机或安全密钥登录。关闭浏览器后需要重新验证。</p>
    <p v-if="error" class="notice error">{{ error }}</p>
    <div class="account-actions">
      <button :disabled="busy" @click="emit('login')">使用 passkey 登录</button>
      <button class="secondary" :disabled="busy" @click="emit('create')">创建新账号</button>
    </div>
    <button class="text-button" :disabled="busy" @click="emit('loginOther')">使用其他 passkey</button>
    <button class="text-button" :disabled="busy" @click="recovering = !recovering">
      {{ recovering ? '取消恢复' : '使用账号恢复码' }}
    </button>
    <form v-if="recovering" class="recovery-form" @submit.prevent="emit('recover', recoveryCode)">
      <label>
        <span>账号恢复码</span>
        <input v-model.trim="recoveryCode" autocomplete="off" placeholder="RM1-..." required>
      </label>
      <button :disabled="busy || !recoveryCode">在本机创建 passkey</button>
    </form>
  </section>
</template>
