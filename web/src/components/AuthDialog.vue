<template>
  <el-dialog
      v-model="visible"
      :title="translate('auth.title')"
      width="440px"
      :close-on-click-modal="true"
      @closed="onClosed"
  >
    <p class="text-sm text-gray-600 mb-3">{{ translate('auth.description') }}</p>

    <el-alert
        v-if="authState.unauthorized"
        class="mb-3"
        type="warning"
        :title="translate('auth.required')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else-if="!hasToken"
        class="mb-3"
        type="info"
        :title="translate('auth.labMode')"
        :closable="false"
        show-icon
    />

    <el-form label-width="auto">
      <el-form-item :label="translate('auth.tokenLabel')">
        <el-input
            v-model="authState.draftToken"
            type="password"
            show-password
            clearable
            :placeholder="translate('auth.tokenPlaceholder')"
            @keydown.enter="onLogin"
        />
      </el-form-item>
    </el-form>

    <template #footer>
      <el-button @click="closeAuthDialog">{{ translate('common.cancel') }}</el-button>
      <el-button v-if="hasToken" type="danger" plain @click="onLogout">
        {{ translate('auth.logout') }}
      </el-button>
      <el-button type="primary" :disabled="!authState.draftToken.trim()" @click="onLogin">
        {{ translate('auth.login') }}
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup lang="ts">
import {computed} from "vue";
import {translate} from "@/locales";
import {
  authState,
  clearAuthToken,
  closeAuthDialog,
  hasToken,
  saveAuthToken,
} from "@/composables/useAuthSession";

/** dialogVisible <-> el-dialog v-model（关闭时只收起，不自动清除 token） */
const visible = computed({
  get: () => authState.dialogVisible,
  set: (value: boolean) => {
    authState.dialogVisible = value;
  },
});

function onLogin() {
  saveAuthToken(authState.draftToken);
}

function onLogout() {
  clearAuthToken();
}

function onClosed() {
  authState.draftToken = authState.token;
}
</script>
