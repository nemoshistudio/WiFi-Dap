<template>
  <div class="text-layout">
    <h1 class="page-title">
      Wi-Fi {{ translate('wifi.settings') }}
    </h1>
    <el-divider></el-divider>

    <h2 class="mb-4 text-xl font-bold tracking-tight md:text-2xl lg:text-3xl">{{ translate('wifi.connection') }} Wi-Fi</h2>
    <el-form label-width="auto" ref="formRef" :model="ssidValidateForm" class="m-auto">
      <el-form-item
          label="Wi-Fi"
          prop="wifiSsid"
          :rules="[
        { required: true, message: translate('wifi.warnWifiName')},
      ]"
      >
        <div class="flex w-full">
          <el-autocomplete
              v-model="ssidValidateForm.wifiSsid"
              clearable
              :placeholder="wifiListPlaceholder"
              class="inline-input w-full"
              :fetch-suggestions="querySearch"
              value-key="ssid"
          >
            <template #default="{ item }">
              <div class="flex items-center border-b">
                <InlineSvg :name="item.wifiLogo" class="h-6 pr-4"></InlineSvg>
                <!--                <span class="w-10">{{ item.rssi }}</span>-->
                <div>{{ item.ssid }}</div>
              </div>
            </template>
          </el-autocomplete>
          <div class="h-8">
            <el-button class="h-8" @click="onScanClick">{{ scanText }}</el-button>
          </div>
        </div>
      </el-form-item>
      <el-form-item :label="translate('wifi.password')">
        <el-input
            v-model="ssidValidateForm.password"
            show-password
            type="password"
            clearable
        />
      </el-form-item>
      <div class="mb-2">
        <el-alert type="info" show-icon>
          {{ translate("wifi.connectInfoHTML")}}
        </el-alert>
      </div>
      <div class="flex justify-center">
        <el-button @click="onConnectClick" type="primary">{{ translate('wifi.connect') }}</el-button>
      </div>
    </el-form>

    <el-divider></el-divider>

    <div v-if="savedSupported" class="mb-4">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <div class="flex flex-wrap items-center gap-2">
          <h2 class="text-xl font-bold tracking-tight md:text-2xl">
            {{ translate('wifi.saved.title') }}
          </h2>
          <el-tooltip effect="light" :content="translate('wifi.saved.autoSaveHint')" :show-after="500">
            <el-switch
                v-model="savedAutoSave"
                :active-text="translate('wifi.saved.autoSave')"
                :disabled="!isConnected"
                @change="onAutoSaveChange"
            />
          </el-tooltip>
        </div>
        <div class="flex gap-2">
          <el-button :disabled="!isConnected || savedLoading" @click="refreshSavedList">
            {{ translate('common.refresh') }}
          </el-button>
          <el-tooltip
              :content="listFullTooltip"
              :disabled="!savedMaxReached"
              :show-after="300"
          >
            <span class="inline-flex">
              <el-button type="primary" :disabled="!isConnected || savedMaxReached" @click="openAddSavedDialog">
                {{ translate('wifi.saved.add') }}
              </el-button>
            </span>
          </el-tooltip>
        </div>
      </div>
      <p class="text-sm text-gray-600 mt-1">{{ translate('wifi.saved.description') }}</p>

      <el-empty v-if="!savedList.length" :description="translate('wifi.saved.empty')" :image-size="80"/>
      <el-table v-else :data="savedList" size="small" class="mt-2" border>
        <el-table-column label="SSID" min-width="150">
          <template #default="{ row }">
            <span>{{ row.ssid }}</span>
            <el-tag v-if="isCurrentSavedSsid(row.ssid)" size="small" type="success" class="ml-2">
              {{ translate('wifi.saved.current') }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column :label="translate('wifi.saved.lastSuccess')" min-width="150">
          <template #default="{ row }">{{ formatLastSuccess(row.last_success) }}</template>
        </el-table-column>
        <el-table-column :label="translate('wifi.saved.priority')" width="90" prop="priority"/>
        <el-table-column :label="translate('common.actions')" width="210">
          <template #default="{ row }">
            <el-button
                size="small"
                type="primary"
                plain
                :loading="savedConnectingIdx === row.idx"
                :disabled="!isConnected"
                @click="openConnectSavedDialog(row)"
            >
              {{ translate('wifi.saved.connect') }}
            </el-button>
            <el-button size="small" type="danger" plain :disabled="!isConnected" @click="onForgetSaved(row)">
              {{ translate('wifi.saved.forget') }}
            </el-button>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <el-divider></el-divider>
    <div class="flex items-center">
      <h5 class="text-md font-bold text-gray-800 w-32">Wi-Fi {{ translate('wifi.mode') }}</h5>
      <div class="flex shrink-0">
        <el-tooltip effect="light">
          <template #content>
            <div v-html="translate('wifi.modeTipsHtml')"></div>
          </template>
          <InlineSvg name="help" class="w-3.5 h-3.5 text-gray-500 cursor-help"></InlineSvg>
        </el-tooltip>
      </div>
      <el-select v-model="wifiMode" :disabled="wsStore.state != ControlEvent.CONNECTED">
        <el-option
            v-for="item in wifiModeOptions"
            :key="item.key"
            :value="item.key"
            :label="item.label"
        />
      </el-select>
      <el-button type="primary" @click="wifiChangeMode" :loading="wifiMode_loading">{{ translate('wifi.save') }}</el-button>
    </div>

    <el-divider></el-divider>


    <el-descriptions
        :column="1"
        border
        class="description-style"
    >
      <template #title>
        Wi-Fi {{ translate('wifi.stationInfo') }}
        <el-tag v-if="!isConnected" type="danger">{{ translate('wifi.disconnected') }}</el-tag>
      </template>
      <template #extra>
        <el-switch v-model="wifiSta_On" :disabled="!isConnected || !wifiAp_On"
                   :active-text="translate('wifi.enabled')" :inactive-text="translate('wifi.disabled')" :loading="wifiMode_loading"
                   :before-change="()=>beforeWifiModeChange('STA')"
        />
      </template>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.signalStrength') }}
          </div>
        </template>
        <template #default>
          <p> {{ wifi_rssi_to_percent(wifiStaApInfo.rssi) }} % ({{ wifiStaApInfo.rssi }} dBm)</p>
        </template>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            Wi-Fi(SSID)
          </div>
        </template>
        <p>{{ wifiStaApInfo.ssid }}</p>
      </el-descriptions-item>
<!--      <el-descriptions-item span="6" >-->
<!--        <template #label>-->
<!--          <div>-->
<!--            密码-->
<!--          </div>-->
<!--        </template>-->
<!--        <password-viewer :password="wifiStaApInfo.password"></password-viewer>-->
<!--      </el-descriptions-item>-->
      <el-descriptions-item span="4">
        <template #label>
          <div>
            MAC
          </div>
        </template>
        <p>{{ wifiStaApInfo.mac }}</p>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>IP({{ translate('wifi.internalAddress') }})</div>
        </template>
        <p>{{ wifiStaApInfo.ip }}</p>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.gateway') }}
          </div>
        </template>
        <p>{{ wifiStaApInfo.gateway }}</p>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.netmask') }}
          </div>
        </template>
        <p>{{ wifiStaApInfo.netmask }}</p>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.primaryDNS') }}
          </div>
        </template>
        <p>{{ wifiStaApInfo.dns_main }}</p>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.backupDNS') }}
          </div>
        </template>
        <p>{{ wifiStaApInfo.dns_backup }}</p>
      </el-descriptions-item>

      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.IPmode') }}
          </div>
        </template>
        <el-select v-model="wifiStaticInfo.static_ip_en" :disabled="!isConnected">
          <el-option
              v-for="item in staIPModeOptions"
              :key="item.key"
              :value="item.key"
              :label="item.label"
          />
        </el-select>
      </el-descriptions-item>
      <el-descriptions-item span="4" v-if="wifiStaticInfo.static_ip_en">
        <template #label>
          <div>IP({{ translate('wifi.internalAddress') }})</div>
        </template>
        <el-input v-model="wifiStaticInfo.ip"></el-input>
      </el-descriptions-item>
      <el-descriptions-item span="4" v-if="wifiStaticInfo.static_ip_en">
        <template #label>
          <div>
            {{ translate('wifi.gateway') }}
          </div>
        </template>
        <el-input v-model="wifiStaticInfo.gateway"></el-input>
      </el-descriptions-item>
      <el-descriptions-item span="4" v-if="wifiStaticInfo.static_ip_en">
        <template #label>
          <div>
            {{ translate('wifi.netmask') }}
          </div>
        </template>
        <el-input v-model="wifiStaticInfo.netmask"></el-input>
      </el-descriptions-item>

      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.DNSmode') }}
          </div>
        </template>
        <el-select v-model="wifiStaticInfo.static_dns_en" :disabled="!isConnected">
          <el-option
              v-for="item in staDNSModeOptions"
              :key="item.key"
              :value="item.key"
              :label="item.label"
          />
        </el-select>
      </el-descriptions-item>
      <el-descriptions-item span="4" v-if="wifiStaticInfo.static_dns_en">
        <template #label>
          <div>
            {{ translate('wifi.primaryDNS') }}
          </div>
        </template>
        <el-input v-model="wifiStaticInfo.dns_main"></el-input>
      </el-descriptions-item>
      <el-descriptions-item span="4" v-if="wifiStaticInfo.static_dns_en">
        <template #label>
          <div>
            {{ translate('wifi.backupDNS') }}
          </div>
        </template>
        <el-input v-model="wifiStaticInfo.dns_backup"></el-input>
      </el-descriptions-item>
    </el-descriptions>
    <div class="flex justify-center mt-4">
      <el-button type="primary" :loading="wifiMode_loading" @click="wifiStaSetStaticInfo">{{ translate('wifi.save') }}</el-button>
    </div>

    <el-divider></el-divider>

    <el-descriptions
        :column="1"
        border
        class="description-style"
    >
      <template #title>
        Wi-Fi {{ translate('wifi.hotspotInfo') }}
        <el-tag v-if="!isConnected" type="danger">{{ translate('wifi.disconnected') }}</el-tag>
      </template>
      <template #extra>
        <el-switch v-model="wifiAp_On" :disabled="!isConnected || !wifiSta_On"
                   :loading="wifiMode_loading" :active-text="translate('wifi.enabled')" :inactive-text="translate('wifi.disabled')"
                   :before-change="()=>beforeWifiModeChange('AP')"
        />
      </template>
      <el-descriptions-item span="6">
        <template #label>
          <div>
            Wi-Fi(SSID)
          </div>
        </template>
        <div class="w-full">
          <div class="flex items-center">
            <el-input v-model="wifiApInfo.ssid"></el-input>
            <el-tag class="ml-2 shrink-0" size="small" :type="isDefaultApSsid ? 'info' : 'warning'">
              {{ isDefaultApSsid ? translate('wifi.saved.apDefault') : translate('wifi.saved.apCustom') }}
            </el-tag>
          </div>
          <p class="text-xs text-gray-500 mt-1">{{ translate('wifi.saved.apDefaultHint') }}</p>
        </div>
      </el-descriptions-item>
      <el-descriptions-item span="6">
        <template #label>
          <div>
            {{ translate('wifi.password') }}
          </div>
        </template>
        <el-input v-model="wifiApInfo.password"></el-input>
      </el-descriptions-item>
      <el-descriptions-item span="4">
        <template #label>
          <div>
            IP
          </div>
        </template>
        {{ wifiApInfo.ip }}
      </el-descriptions-item>

      <el-descriptions-item span="4">
        <template #label>
          <div>
            MAC
          </div>
        </template>
        {{ wifiApInfo.mac }}
      </el-descriptions-item>

      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.gateway') }}
          </div>
        </template>
        {{ wifiApInfo.gateway }}
      </el-descriptions-item>

      <el-descriptions-item span="4">
        <template #label>
          <div>
            {{ translate('wifi.netmask') }}
          </div>
        </template>
        {{ wifiApInfo.netmask }}
      </el-descriptions-item>
    </el-descriptions>
    <div class="flex justify-center gap-2 mt-4">
      <el-button :loading="wifiMode_loading" @click="onResetApDefault">{{ translate('wifi.ap.resetDefault') }}</el-button>
      <el-button type="primary" :loading="wifiMode_loading" @click="wifiApChangeCredential">{{ translate('wifi.save') }}</el-button>
    </div>
    <el-divider></el-divider>

    <el-dialog v-model="savedAddVisible" :title="translate('wifi.saved.addTitle')" width="420px">
      <el-form label-width="auto">
        <el-form-item label="Wi-Fi">
          <el-input v-model="savedAddForm.ssid" :maxlength="32" clearable/>
        </el-form-item>
        <el-form-item :label="translate('wifi.password')">
          <el-input v-model="savedAddForm.password" type="password" show-password clearable/>
        </el-form-item>
        <el-form-item :label="translate('wifi.saved.priority')">
          <el-input-number v-model="savedAddForm.priority" :min="0" :max="255"/>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="savedAddVisible = false">{{ translate('common.cancel') }}</el-button>
        <el-button type="primary" :loading="savedAddSaving" @click="submitAddSaved">
          {{ translate('wifi.saved.add') }}
        </el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="savedConnectVisible" :title="translate('wifi.saved.connect')" width="420px">
      <el-form label-width="auto">
        <el-form-item label="Wi-Fi">
          <el-input v-model="savedConnectForm.ssid" disabled/>
        </el-form-item>
        <el-form-item :label="translate('wifi.password')">
          <el-input
              v-model="savedConnectForm.password"
              type="password"
              show-password
              clearable
              @keydown.enter="submitConnectSaved"
          />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="savedConnectVisible = false">{{ translate('common.cancel') }}</el-button>
        <el-button type="primary" :loading="savedConnectingIdx >= 0" @click="submitConnectSaved">
          {{ translate('wifi.saved.connect') }}
        </el-button>
      </template>
    </el-dialog>
  </div>

</template>

<script setup lang="ts">
import {computed, type ComputedRef, onMounted, onUnmounted, reactive, ref} from "vue";
import {
  type IWifiMode,
  wifi_ap_get_info,
  wifi_ap_set_credential,
  wifi_connect_to,
  wifi_get_mode,
  wifi_get_scan_list,
  wifi_set_mode,
  wifi_sta_get_ap_info,
  WifiCmd,
  type WifiInfo,
  type WifiList,
  WifiMode,
  type WifiScanInfo,
  type WiFiCredential,
  WifiStatus, wifi_sta_get_static_info,
  type IWifiStaStaticInfo, wifi_sta_set_static_conf,
  type IWifiSavedItem,
  type IWifiSavedAddResult,
  type IWifiAutoSaveResult,
  type IWifiSavedList,
  wifi_get_saved_list,
  wifi_add_saved,
  wifi_del_saved,
  wifi_set_auto_save,
} from "@/api/apiWifi";
import {ElMessageBox, type FormInstance} from "element-plus";

import InlineSvg from "@/components/InlineSvg.vue";
import type {ApiJsonMsg, ControlMsg} from "@/api";
import {ControlEvent, ControlMsgType, WtModuleID} from "@/api";
import {registerModule, unregisterModule} from "@/router/msgRouter";
import {useWsStore} from "@/stores/websocket";
import {globalNotify, globalNotifyRightSide} from "@/composables/notification";
import {isDevMode} from "@/composables/buildMode";
import {translate} from "@/locales";

const formRef = ref<FormInstance>()
let wifiListPlaceholder = ref("MY-WIFI")
let ssidValidateForm = reactive({
  wifiSsid: "",
  password: "",
})

let wifiSta_On = ref(false);
const wifiMode_loading = ref(false)

let wifiAp_On = ref(false);

let wifiMode = ref(-1);

let wifiModeOptions = computed( () => [
  {
    label: translate('wifi.APauto_STA'),
    key: WifiMode.WIFI_AP_AUTO_STA_ON,
  }, {
    label: translate('wifi.APonly'),
    key: WifiMode.WIFI_AP_ON_STA_OFF,
  }, {
    label: translate('wifi.AP_STA'),
    key: WifiMode.WIFI_AP_STA_ON,
  }, /*
   {
    value: "仅开启终端（STA）",
    key: 2,
  },*/
])

let wsStore = useWsStore();

const defWifiInfo: WifiInfo = {
  cmd: 1,
  module: 1,
  gateway: "-",
  ip: "-",
  mac: "-",
  dns_main: "-",
  dns_backup: "-",
  rssi: 0,
  netmask: "-",
  ssid: "-",
  password: "",
}

const staIPModeOptions = [
  {
    label: translate('wifi.autoIP'),
    key: 0,
  }, {
    label: translate('wifi.staticIP'),
    key: 1,
  },
]

const staDNSModeOptions = [
  {
    label: translate('wifi.autoDNS'),
    key: 0,
  }, {
    label: translate('wifi.staticDNS'),
    key: 1,
  },
]

const isConnected = computed(() => wsStore.state === ControlEvent.CONNECTED)
let wifiStaApInfo = reactive<WifiInfo>({...defWifiInfo});
let wifiApInfo = reactive<WifiInfo>({...defWifiInfo});
let wifiStaticInfo = reactive<IWifiStaStaticInfo>({
  dns_backup: "0.0.0.0",
  dns_main: "0.0.0.0",
  gateway: "0.0.0.0",
  ip: "0.0.0.0",
  netmask: "0.0.0.0",
  static_dns_en: 0,
  static_ip_en: 0,
});

/* ---- 多 WiFi 凭据记忆（docs/wifi-credential-memory.md §7） ---- */
/** 旧固件 GET_SAVED_LIST 缺少 max 字段时的契约默认值 8（前端不得硬编码为主逻辑） */
const SAVED_WIFI_DEFAULT_MAX = 8;
/** 设备侧保存上限：优先取 GET_SAVED_LIST.max（F4）；旧固件回退 8 */
let savedMax = ref(SAVED_WIFI_DEFAULT_MAX);
/** 添加网络请求前的列表快照，用于把 evicted_idx 映射回被淘汰的 SSID（F2） */
let pendingAddSnapshot: IWifiSavedItem[] = [];
let savedSupported = ref(true);
let savedList = ref<IWifiSavedItem[]>([]);
let savedAutoSave = ref(true);
let savedLoading = ref(false);
let savedAddVisible = ref(false);
let savedAddSaving = ref(false);
let savedAddForm = reactive({
  ssid: "",
  password: "",
  priority: 100,
});
let savedConnectVisible = ref(false);
let savedConnectingIdx = ref(-1);
let savedConnectForm = reactive({
  idx: -1,
  ssid: "",
  password: "",
});
const savedMaxReached = computed(() => savedList.value.length >= savedMax.value);
/** 列表满时“添加网络”按钮的 tooltip（F2，占位 {max}） */
const listFullTooltip = computed(() => translate('wifi.saved.listFull', {max: savedMax.value}));
/** AP SSID 是否仍为默认 WiFi-Dap-XXXX（用户自定义优先，空串=恢复默认） */
const isDefaultApSsid = computed(() => {
  const ssid = wifiApInfo.ssid;
  return ssid === "" || /^WiFi-Dap-[0-9A-Fa-f]{4}$/.test(ssid);
});

let scanning = ref(false);
let scan_cb: any;
let connectBtnClicked = 0;
let options: Array<WifiScanInfo> = [];
const scanText = computed(() => {
  return scanning.value ? translate("wifi.scanning") : translate("wifi.scan");
});

const querySearch = (queryString: string, cb: any) => {
  if (scanning.value) {
    scan_cb = cb;
  } else {
    cb(options);
  }
}

const onClientMsg = (msg: ApiJsonMsg) => {
  switch (msg.cmd as WifiCmd) {
    case WifiCmd.UNKNOWN:
      break;
    case WifiCmd.WIFI_API_JSON_STA_GET_AP_INFO: {
      const info = msg as WifiInfo;
      if (info.rssi === 0) {
        Object.assign(wifiStaApInfo, defWifiInfo);
      } else {
        Object.assign(wifiStaApInfo, info);
      }
      if (connectBtnClicked) {
        connectBtnClicked = 0;
        savedConnectingIdx.value = -1;
        savedConnectVisible.value = false;
        globalNotifyRightSide(wifiStaApInfo.ssid + " " + translate('wifi.connectionSuccess'), "success");
        wifi_sta_get_static_info();
      }
      break;
    }
    case WifiCmd.WIFI_API_JSON_CONNECT: {
      if (msg.err !== undefined) {
        connectBtnClicked = 0;
        savedConnectingIdx.value = -1;
        globalNotifyRightSide(translate('wifi.saved.connectFailed', {err: msg.err}), "error");
        break;
      }
      // 成功响应复用 GET_STA_INFO 字段；主动拉一次以统一走 cmd1 的成功提示/刷新逻辑
      wifi_sta_get_ap_info();
      break;
    }
    case WifiCmd.WIFI_API_JSON_GET_SCAN: {
      const list = msg as WifiList;
      scanning.value = false;
      list.scan_list.forEach(value => {
        if (value.rssi > -50) {
          value.wifiLogo = "wifi-3";
        } else if (value.rssi > -65) {
          value.wifiLogo = "wifi-2";
        } else {
          value.wifiLogo = "wifi-1";
        }
      });
      options = list.scan_list;
      if (scan_cb) {
        scan_cb(options);
        scan_cb = null;
      }
      globalNotifyRightSide(translate('wifi.scanDone'), "success");
      break;
    }
    case WifiCmd.WIFI_API_JSON_DISCONNECT:
      break;
    case WifiCmd.WIFI_API_JSON_AP_GET_INFO: {
      const info = msg as WifiInfo;
      Object.assign(wifiApInfo, info);
      break;
    }
    case WifiCmd.WIFI_API_JSON_SET_MODE:
      wifi_get_mode();
      /* falls through */
    case WifiCmd.WIFI_API_JSON_GET_MODE: {
      const modeInfo = msg as IWifiMode;
      wifiMode_loading.value = false;
      if (modeInfo.err !== undefined) {
        globalNotifyRightSide(translate('wifi.setFailed'), "error");
        return;
      }

      if (modeInfo.status !== undefined) {
        wifiAp_On.value = modeInfo.status === WifiStatus.WIFI_MODE_AP || modeInfo.status === WifiStatus.WIFI_MODE_APSTA;
        wifiSta_On.value = modeInfo.status === WifiStatus.WIFI_MODE_STA || modeInfo.status === WifiStatus.WIFI_MODE_APSTA;
      }
      if (modeInfo.mode !== undefined) {
        if (modeInfo.mode < WifiMode.WIFI_AP_STOP) {
          wifiMode.value = modeInfo.mode;
        } else if (modeInfo.mode === WifiMode.WIFI_AP_START) {
          wifiAp_On.value = true;
        } else if (modeInfo.mode === WifiMode.WIFI_AP_STOP) {
          wifiAp_On.value = false;
        } else if (modeInfo.mode === WifiMode.WIFI_STA_START) {
          wifiSta_On.value = true;
        } else if (modeInfo.mode === WifiMode.WIFI_STA_STOP) {
          wifiSta_On.value = false;
        }
      }
      break;
    }
    case WifiCmd.WIFI_API_JSON_AP_SET_CRED: {
      const wifiCred = msg as WiFiCredential;
      if (wifiCred.err !== undefined) {
        globalNotifyRightSide(savedErrorText(msg), "error");
      } else {
        globalNotifyRightSide(translate('wifi.setSuccess'), "success");
        // SET_AP_CRED 成功后刷新生效 AP 信息与已保存列表（含“恢复默认 SSID”场景，F3）
        wifi_ap_get_info();
        refreshSavedList();
      }
      wifiMode_loading.value = false;

      break;
    }
    case WifiCmd.WIFI_API_JSON_STA_GET_STATIC_INFO: {
      const staticInfo = msg as IWifiStaStaticInfo & ApiJsonMsg;
      Object.assign(wifiStaticInfo, staticInfo);
      break;
    }
    case WifiCmd.WIFI_API_JSON_STA_SET_STATIC_CONF:
      wifiMode_loading.value = false;
      break;
    case WifiCmd.WIFI_API_JSON_GET_SAVED_LIST: {
      savedLoading.value = false;
      if (msg.err !== undefined) {
        // 旧固件不支持 cmd 11–14：优雅降级，整块隐藏（契约 §9.1-2）
        if (msg.code === 4) {
          savedSupported.value = false;
        } else {
          globalNotifyRightSide(savedErrorText(msg), "error");
        }
        break;
      }
      const saved = msg as IWifiSavedList;
      savedSupported.value = true;
      savedAutoSave.value = saved.auto_save !== false;
      savedList.value = Array.isArray(saved.list) ? saved.list : [];
      // F4：设备上限优先，不得硬编码 8；旧固件缺 max 时回退契约默认值
      savedMax.value = typeof saved.max === "number" && saved.max > 0
          ? saved.max
          : SAVED_WIFI_DEFAULT_MAX;
      break;
    }
    case WifiCmd.WIFI_API_JSON_ADD_SAVED: {
      savedAddSaving.value = false;
      if (msg.err !== undefined) {
        pendingAddSnapshot = [];
        globalNotifyRightSide(savedErrorText(msg), "error");
        break;
      }
      const addResult = msg as IWifiSavedAddResult;
      savedAddVisible.value = false;
      // F2：表满淘汰时用请求前快照把 evicted_idx 映射回被淘汰的 SSID，明确提示
      if (typeof addResult.evicted_idx === "number") {
        const evicted = pendingAddSnapshot.find((item) => item.idx === addResult.evicted_idx);
        if (evicted) {
          globalNotifyRightSide(translate('wifi.saved.evictedHint', {ssid: evicted.ssid}), "warning");
        }
      }
      pendingAddSnapshot = [];
      if (addResult.updated) {
        globalNotifyRightSide(translate('wifi.saved.updated'), "success");
      } else {
        globalNotifyRightSide(translate('wifi.saved.added'), "success");
      }
      refreshSavedList();
      break;
    }
    case WifiCmd.WIFI_API_JSON_DEL_SAVED: {
      if (msg.err !== undefined) {
        globalNotifyRightSide(savedErrorText(msg), "error");
        break;
      }
      globalNotifyRightSide(translate('wifi.saved.deleted'), "success");
      refreshSavedList();
      break;
    }
    case WifiCmd.WIFI_API_JSON_SET_AUTO_SAVE: {
      if (msg.err !== undefined) {
        globalNotifyRightSide(savedErrorText(msg), "error");
        // 失败时回读设备状态，避免开关与设备不一致
        refreshSavedList();
        break;
      }
      const autoSaveResult = msg as IWifiAutoSaveResult;
      if (typeof autoSaveResult.auto_save === "boolean") {
        savedAutoSave.value = autoSaveResult.auto_save;
      }
      break;
    }
    default:
      if (isDevMode()) {
        console.log(msg);
      }
      break;
  }
};

const onClientCtrl = (msg: ControlMsg) => {
  if (msg.type !== ControlMsgType.WS_EVENT) {
    return
  }

  if (msg.data === ControlEvent.DISCONNECTED) {
    Object.assign(wifiStaApInfo, defWifiInfo);
    Object.assign(wifiApInfo, defWifiInfo);
    savedConnectingIdx.value = -1;
  }

  if (msg.data === ControlEvent.CONNECTED) {
    wifi_sta_get_ap_info();
    wifi_ap_get_info();
    wifi_get_mode();
    wifi_sta_get_static_info();
    refreshSavedList();
  }
};

function onScanClick() {
  if (!isConnected.value) {
    globalNotify(translate('wifi.debuggerNotConnected'), 'error');
    return;
  }
  scanning.value = true;
  wifi_get_scan_list();
}

function onConnectClick() {
  if (!isConnected.value) {
    globalNotify(translate('wifi.debuggerNotConnected'), 'error');
    return;
  }
  if (ssidValidateForm.wifiSsid !== "") {
    wifi_connect_to(ssidValidateForm.wifiSsid, ssidValidateForm.password);
    connectBtnClicked = 1;
  }
}

function beforeWifiModeChange(ap_sta: "AP" | "STA" = "AP") {
  if (ap_sta === "AP") {
    wifiMode_loading.value = true;
    wifi_set_mode(wifiAp_On.value ? WifiMode.WIFI_AP_STOP : WifiMode.WIFI_AP_START);
  } else {
    wifiMode_loading.value = true;
    wifi_set_mode(wifiSta_On.value ? WifiMode.WIFI_STA_STOP : WifiMode.WIFI_STA_START);
  }
  wifi_sta_get_ap_info();
  return false;
}

function wifiChangeMode() {
  wifiMode_loading.value = true;
  wifi_set_mode(wifiMode.value);
}

function wifi_rssi_to_percent(rssi: number)
{
  if (rssi <= -100) {
    return 0;
  } else if (rssi >= -50) {
    return 100;
  } else {
    return 2 * (rssi + 100);
  }
}

function wifiApChangeCredential() {
  if (wifiApInfo.ssid === "") {
    globalNotifyRightSide(translate('wifi.enterAPName'), "error");
    return;
  }
  wifiMode_loading.value = true;
  wifi_ap_set_credential(wifiApInfo.ssid, wifiApInfo.password);
}

/** F3：恢复默认 AP SSID（SET_AP_CRED cmd8，ssid="" → 设备按 MAC 重新计算 WiFi-Dap-XXXX） */
async function onResetApDefault() {
  try {
    await ElMessageBox.confirm(
        translate('wifi.ap.resetConfirm'),
        translate('wifi.ap.resetDefault'),
        {
          confirmButtonText: translate('common.confirm'),
          cancelButtonText: translate('common.cancel'),
          type: 'warning',
        },
    );
  } catch {
    return;
  }
  if (!isConnected.value) {
    globalNotify(translate('wifi.debuggerNotConnected'), "error");
    return;
  }
  wifiMode_loading.value = true;
  // 空 ssid = 恢复默认动态 SSID；口令沿用设备当前生效值，避免误改
  wifi_ap_set_credential("", wifiApInfo.password);
}

function wifiStaSetStaticInfo() {
  wifiMode_loading.value = true;
  wifi_sta_set_static_conf(wifiStaticInfo);
  wifi_sta_get_ap_info();
}

/* ---- 多 WiFi 凭据记忆 UI（WP-208） ---- */

/**
 * 已保存网络命令的错误提示映射（docs/wifi-credential-memory.md §7.6/§9.2）。
 * 设备 err 为英文原始信息，不参与 i18n；dev 模式下打印便于排查。
 */
function savedErrorText(msg: ApiJsonMsg): string {
  if (msg.code === 4) {
    return translate('common.unsupported');
  }
  if (isDevMode() && msg.err) {
    console.warn('[wifi] saved-network error:', msg.err);
  }
  return translate('common.failed');
}

function refreshSavedList() {
  if (!savedSupported.value) {
    return;
  }
  savedLoading.value = true;
  wifi_get_saved_list();
}

function formatLastSuccess(timestamp: number): string {
  if (!timestamp || timestamp <= 0) {
    // 设备未对时或从未成功（契约 §7.1）
    return translate('common.unknown');
  }
  return new Date(timestamp * 1000).toLocaleString();
}

function isCurrentSavedSsid(ssid: string): boolean {
  return wifiStaApInfo.rssi !== 0 && wifiStaApInfo.ssid === ssid;
}

function onAutoSaveChange(value: string | number | boolean) {
  wifi_set_auto_save(Boolean(value));
}

function openAddSavedDialog() {
  savedAddForm.ssid = "";
  savedAddForm.password = "";
  savedAddForm.priority = 100;
  savedAddVisible.value = true;
}

function submitAddSaved() {
  const ssid = savedAddForm.ssid.trim();
  const ssidBytes = new TextEncoder().encode(ssid).length;
  if (ssidBytes === 0 || ssidBytes > 32) {
    globalNotifyRightSide(translate('wifi.warnWifiName'), "error");
    return;
  }
  const passwordBytes = new TextEncoder().encode(savedAddForm.password).length;
  // 开放网络（0 字节）或 WPA 口令 8–64 字节（契约 §7.2）
  if (passwordBytes !== 0 && (passwordBytes < 8 || passwordBytes > 64)) {
    globalNotifyRightSide(translate('wifi.saved.invalidPassword'), "warning");
    return;
  }
  if (!isConnected.value) {
    globalNotify(translate('wifi.debuggerNotConnected'), "error");
    return;
  }
  // F2：表满时仅允许更新已存在的 SSID；新增会被设备淘汰旧记录，需先提示上限
  const isExisting = savedList.value.some((item) => item.ssid === ssid);
  if (!isExisting && savedMaxReached.value) {
    globalNotifyRightSide(translate('wifi.saved.listFull', {max: savedMax.value}), "warning");
    return;
  }
  pendingAddSnapshot = [...savedList.value];
  savedAddSaving.value = true;
  wifi_add_saved(ssid, savedAddForm.password, savedAddForm.priority);
}

async function onForgetSaved(item: IWifiSavedItem) {
  try {
    await ElMessageBox.confirm(
        translate('wifi.saved.forgetConfirm'),
        translate('wifi.saved.forgetConfirmTitle'),
        {
          confirmButtonText: translate('common.confirm'),
          cancelButtonText: translate('common.cancel'),
          type: 'warning',
        },
    );
  } catch {
    return;
  }
  wifi_del_saved(item.idx);
}

function openConnectSavedDialog(item: IWifiSavedItem) {
  savedConnectForm.idx = item.idx;
  savedConnectForm.ssid = item.ssid;
  savedConnectForm.password = "";
  savedConnectVisible.value = true;
}

function submitConnectSaved() {
  if (savedConnectingIdx.value >= 0) {
    return;
  }
  if (!isConnected.value) {
    globalNotify(translate('wifi.debuggerNotConnected'), "error");
    return;
  }
  if (!savedConnectForm.ssid) {
    return;
  }
  savedConnectingIdx.value = savedConnectForm.idx;
  savedConnectVisible.value = false;
  connectBtnClicked = 1;
  // GET_SAVED_LIST 不回传口令（安全例外），重连已保存网络时由用户重新输入
  wifi_connect_to(savedConnectForm.ssid, savedConnectForm.password);
}

onMounted(() => {
  registerModule(WtModuleID.WIFI, {
    ctrlCallback: onClientCtrl,
    serverJsonMsgCallback: onClientMsg,
    serverBinMsgCallback: () => {},
  });
  wifi_sta_get_ap_info();
  wifi_ap_get_info();
  wifi_get_mode();
  wifi_sta_get_static_info();
  wifi_get_saved_list();
});

onUnmounted(() => {
  unregisterModule(WtModuleID.WIFI);
});

</script>


<style scoped lang="postcss">
.description-style :deep(.el-descriptions__label) {
  @apply w-32
}
</style>
