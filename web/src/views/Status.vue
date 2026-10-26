<template>
  <div class="text-layout bb-page bb-page--wide" data-testid="status-page">
    <h1 class="page-title">{{ translate('page.status') }}</h1>
    <p class="text-center text-xs bb-muted">{{ translate('status.subtitle') }}</p>

    <!-- Read-only detail cards: DAP / WiFi / target / link fusion + link health + errors.
         Data comes from the shared dashboardState telemetry (no extra WS/polling). -->
    <StatusDashboard @open-diagnostics="scrollToDiagnostics" />

    <!-- Detailed diagnostics / recovery: DAP & SWD fields, heap, WiFi, target errors. -->
    <DiagnosticsPanel ref="diagnosticsRef" />
  </div>
</template>

<script setup lang="ts">
import { nextTick, onMounted, onUnmounted, ref } from 'vue';
import { translate } from '@/locales';
import StatusDashboard from '@/components/dashboard/StatusDashboard.vue';
import DiagnosticsPanel from '@/components/dashboard/DiagnosticsPanel.vue';
import { onAction } from '@/components/dashboard/actionBus';
import { startLinkPolling, stopLinkPolling } from '@/components/dashboard/dashboardState';

const diagnosticsRef = ref<InstanceType<typeof DiagnosticsPanel> | null>(null);
let offAction: (() => void) | null = null;

function scrollToDiagnostics(): void {
  void nextTick(() => {
    const el = diagnosticsRef.value?.$el as HTMLElement | undefined;
    el?.scrollIntoView({behavior: 'smooth', block: 'start'});
  });
}

onMounted(() => {
  // The single page-level link-health poll now lives here (it was on the target
  // page before the status page split); no other page polls in the background.
  startLinkPolling();
  // Command palette "打开诊断 / 恢复" navigates here and requests the panel.
  offAction = onAction((action) => {
    if (action === 'diagnostics-open') {
      scrollToDiagnostics();
    }
  });
});

onUnmounted(() => {
  offAction?.();
  offAction = null;
  stopLinkPolling();
});
</script>
