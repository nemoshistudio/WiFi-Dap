<script setup lang="ts">
import {translate} from "@/locales";
</script>

<template>
  <div class="text-layout">
    <h1 class="page-title">404</h1>
    <h2 class="text-center">{{ translate('common.unknown') }}</h2>
    <RouterLink to="/">
      <el-card class="text-center">{{ translate('common.back') }}</el-card>
    </RouterLink>
  </div>
</template>

<style scoped>

</style>
