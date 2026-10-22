<script setup lang="ts">
import {translate} from "@/locales";
</script>

<template>
  <div class="text-layout bb-page">
    <section class="bb-card not-found">
      <p class="not-found__code">404</p>
      <h1 class="not-found__title">{{ translate('common.unknown') }}</h1>
      <RouterLink to="/" class="not-found__back">{{ translate('common.back') }}</RouterLink>
    </section>
  </div>
</template>

<style scoped>
.not-found {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--bb-space-3);
  padding: var(--bb-space-7) var(--bb-space-4);
  text-align: center;
}

.not-found__code {
  margin: 0;
  font-family: var(--bb-font-mono);
  font-size: 3.5rem;
  font-weight: 700;
  line-height: 1;
  letter-spacing: 0.04em;
  color: var(--bb-text-muted);
}

.not-found__title {
  margin: 0;
  font-size: var(--bb-font-size-xl);
  font-weight: 650;
  color: var(--bb-text);
}

.not-found__back {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 32px;
  margin-top: var(--bb-space-2);
  padding: 0 var(--bb-space-4);
  border-radius: var(--bb-radius-sm);
  background: var(--bb-accent);
  color: var(--bb-accent-contrast);
  font-size: var(--bb-font-size-md);
  font-weight: 550;
  text-decoration: none;
  transition: background-color var(--bb-duration-fast) var(--bb-ease);
}

.not-found__back:hover {
  background: var(--bb-accent-strong);
}
</style>
