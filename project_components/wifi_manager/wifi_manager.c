#include "wifi_manager.h"
#include "wifi_configuration.h"
#include "wifi_credential.h"
#include "wifi_event_handler.h"
#include "wifi_storage.h"

#include <esp_err.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lwip/ip4_addr.h>
#include <string.h>
#include <assert.h>
#include <driver/gpio.h>
#include "birdiebot_board.h"

#include "ssdp.h"
#include "wifi_api.h"

#define TAG __FILENAME__

/* wifi_manager internal event group */
#define WIFI_EVT_CONNECT_DONE BIT0 /* a connect attempt ended (success or failure) */
#define WIFI_EVT_CANCEL BIT1       /* rotation must stop */
#define WIFI_EVT_RESTART BIT2      /* rotation should start a fresh cycle now */

/* contract wifi-credential-memory.md §5: 20 s per candidate, 5/10/20/30 s backoff */
#define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS 20000
#define WIFI_MANUAL_CONNECT_TIMEOUT_MS 10000
#define WIFI_ROTATION_SCAN_MAX_AP 24
#define WIFI_ROTATION_CANDIDATE_MAX (BIRDIEBOT_WIFI_CRED_MAX * 2)

static const uint32_t s_rotation_backoff_ms[] = {5000, 10000, 20000, 30000};

/* scan_run() result codes */
#define WIFI_SCAN_OK 0
#define WIFI_SCAN_FAILED 1
#define WIFI_SCAN_CANCELED 2
#define WIFI_SCAN_BUSY 3

typedef struct wifi_ctx_t {
	SemaphoreHandle_t lock;
	EventGroupHandle_t events;
	TaskHandle_t task;         /* task to notify for scan / connect results */
	TaskHandle_t rotation_task;

	/* Scan operation state. ctx.lock only protects publishing/clearing this
	 * block; the per-channel waits run WITHOUT the lock so a multi-second scan
	 * never blocks other WiFi commands (finding F7). Only one scan runs at a
	 * time: a Web SCAN stops the rotation task before scanning, and the rotation
	 * task aborts its scan as soon as rotation_cancel is set. */
	wifi_ap_record_t *scan_ap;
	uint16_t scan_total_aps;
	uint8_t scan_max_ap;

	/* Result of the connect attempt in flight (GOT_IP event; NULL = failure). */
	ip_event_got_ip_t *conn_event;

	/* cross task flags */
	volatile uint8_t is_endless_connect;
	volatile uint8_t auto_reconnect;
	volatile uint8_t is_sta_connected;
	volatile uint8_t rotation_cancel;
	volatile uint8_t rotation_suppressed;
	volatile uint8_t rotation_pending; /* disconnect seen while rotation was suppressed */

	TaskHandle_t delayed_stopAP_task;
	TaskHandle_t delayed_startAP_task;
	uint32_t try_connect_count;
	wifi_apsta_mode_e permanent_mode;
	wifi_mode_t mode;
	int ap_on_delay_tick;  /* AP start delay after STA disconnect (Smart APSTA) */
	int ap_off_delay_tick; /* AP stop delay after STA connect (Smart APSTA) */

	/* credential of the connection attempt currently in flight (§4) */
	char pending_ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];
	char pending_password[BIRDIEBOT_WIFI_PASS_MAX + 1];
	bool pending_valid;
	bool pending_persist;
} wifi_ctx_t;

static esp_netif_t *ap_netif;
static esp_netif_t *sta_netif;

static wifi_ctx_t ctx;

/* Buffers for the rotation task only (single rotation task at a time). */
static wifi_ap_record_t s_rotation_scan_aps[WIFI_ROTATION_SCAN_MAX_AP];
static wifi_cred_candidate_t s_rotation_candidates[WIFI_ROTATION_CANDIDATE_MAX];
static char s_rotation_visible[WIFI_ROTATION_CANDIDATE_MAX][BIRDIEBOT_WIFI_SSID_MAX + 1];

static int set_sta_cred(const char *ssid, const char *password);
static int set_initial_sta_cred(void);
static void disconn_handler(void);
static int set_wifi_mode(wifi_apsta_mode_e mode);
static void handle_wifi_connected(void);
static void handle_wifi_disconnected(void);
static void rotation_request_stop(void);
static int scan_run(wifi_ap_record_t *aps, uint16_t max_ap, uint16_t *found,
                    bool cancellable);
static void delayed_set_ap_stop(void *arg);
static void delayed_set_ap_start(void *arg);

static void wifi_led_init(void);
static void wifi_led_set_off(void);
static void wifi_led_set_on(void);

/* ------------------------------------------------------------------------- */
/* DAP latency: keep the Wi-Fi modem awake (docs/wifi-no-power-save.md)       */
/*                                                                           */
/* WIFI_PS_MIN_MODEM is the IDF default: the station sleeps between beacons   */
/* and its RX path is gated by the AP's DTIM schedule.  Every USBIP/WebSocket */
/* DAP transaction then pays an extra wake-up delay (worst case one beacon    */
/* interval, 100 ms by default) on top of the Wi-Fi round trip, which is the  */
/* dominant per-transaction cost of flashing/debugging.  WIFI_PS_NONE keeps   */
/* the radio in active RX and removes that latency bucket.                    */
/*                                                                           */
/* The mode must be (re)applied at several points because the driver can      */
/* reset it to the Kconfig/default value: after esp_wifi_init()/set_mode(),   */
/* after esp_wifi_start(), on every STA (re)connect, and after every          */
/* esp_wifi_set_mode() change.  The helper verifies the readback and logs the */
/* effective mode, so a regression is visible in the boot log instead of      */
/* silently costing latency.  AP+STA: this is a STA power mode; the AP        */
/* (beacon TX) is unaffected, and an AP-only transition is tolerated.         */
/* ------------------------------------------------------------------------- */
static const char *wifi_ps_name(wifi_ps_type_t ps)
{
	switch (ps) {
	case WIFI_PS_NONE:
		return "none";
	case WIFI_PS_MIN_MODEM:
		return "min_modem";
	case WIFI_PS_MAX_MODEM:
		return "max_modem";
	default:
		return "unknown";
	}
}

void wifi_manager_apply_no_power_save(const char *reason)
{
	wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
	const char *where = (reason != NULL) ? reason : "?";
	esp_err_t err;

	err = esp_wifi_set_ps(WIFI_PS_NONE);
	if (err != ESP_OK) {
		/* e.g. AP-only mode: not fatal, the station is stopped anyway. */
		ESP_LOGW(TAG, "WiFi PS: set WIFI_PS_NONE failed at %s: %s", where,
		         esp_err_to_name(err));
		return;
	}
	err = esp_wifi_get_ps(&ps);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "WiFi PS: get_ps failed at %s: %s", where,
		         esp_err_to_name(err));
		return;
	}
	if (ps != WIFI_PS_NONE) {
		ESP_LOGW(TAG, "WiFi PS: readback=%s at %s (expected none)", wifi_ps_name(ps),
		         where);
		return;
	}
	ESP_LOGI(TAG, "WiFi PS: %s (modem sleep off) at %s", wifi_ps_name(ps), where);
}

wifi_ps_type_t wifi_manager_get_power_save(void)
{
	wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;

	if (esp_wifi_get_ps(&ps) != ESP_OK) {
		return WIFI_PS_MIN_MODEM; /* unknown: never report a false "none" */
	}
	return ps;
}

const char *wifi_manager_power_save_name(wifi_ps_type_t ps)
{
	return wifi_ps_name(ps);
}

static bool disconnect_reason_is_credential(int reason)
{
	switch (reason) {
	case WIFI_REASON_AUTH_FAIL:
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_MIC_FAILURE:
		return true;
	default:
		return false;
	}
}

/* Remember the credential that the attempt in flight uses; a successful GOT_IP
 * writes it into the credential table (contract §4). */
static void rotation_prepare_pending(const char *ssid, const char *password, int save)
{
	wifi_cred_ssid_copy(ctx.pending_ssid, ssid);
	wifi_cred_password_copy(ctx.pending_password, password);
	ctx.pending_persist = (save == WIFI_API_CONNECT_SAVE_AUTO)
	                      ? wifi_cred_mem_get_auto_save()
	                      : (save == WIFI_API_CONNECT_SAVE_YES);
	ctx.pending_valid = true;
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

#if defined(CONFIG_IDF_TARGET_ESP32C5)
/*
 * ESP32-C5 has one dual-band Wi-Fi 6 radio. WIFI_BAND_MODE_AUTO means "let the
 * driver pick 2.4 GHz or 5 GHz from the AP/SoftAP configuration" -- it is NOT
 * simultaneous dual-band operation (AP and STA stay on the same band, see the
 * ESP-IDF v5.5 Wi-Fi guide). The default is AUTO so a C5 station can join an
 * existing 5 GHz network; the legacy 2.4 GHz-only mode stays selectable via
 * Kconfig for interoperability debugging.
 *
 * The driver rejects esp_wifi_set_band_mode() before esp_wifi_start()
 * (ESP_ERR_WIFI_NOT_STARTED), so this must run AFTER start -- the earlier
 * compile-only code called it before start and silently kept the default.
 */
static const char *wifi_band_mode_name(wifi_band_mode_t mode)
{
	switch (mode) {
	case WIFI_BAND_MODE_2G_ONLY:
		return "2.4 GHz only";
	case WIFI_BAND_MODE_5G_ONLY:
		return "5 GHz only";
	case WIFI_BAND_MODE_AUTO:
		return "2.4 GHz + 5 GHz (auto)";
	default:
		return "unknown";
	}
}

static void wifi_manager_apply_band_mode(const char *where)
{
#if defined(CONFIG_BIRDIEBOT_WIFI_BAND_MODE_2G_ONLY)
	const wifi_band_mode_t want = WIFI_BAND_MODE_2G_ONLY;
#else
	const wifi_band_mode_t want = WIFI_BAND_MODE_AUTO;
#endif
	wifi_band_mode_t got = WIFI_BAND_MODE_AUTO;
	esp_err_t band_err = esp_wifi_set_band_mode(want);

	if (band_err != ESP_OK) {
		ESP_LOGW(TAG, "Wi-Fi band mode apply failed at %s: %s", where,
		         esp_err_to_name(band_err));
		return;
	}
	if (esp_wifi_get_band_mode(&got) == ESP_OK) {
		ESP_LOGI(TAG, "Wi-Fi band mode: %s at %s", wifi_band_mode_name(got), where);
	} else {
		ESP_LOGI(TAG, "Wi-Fi band mode set to %s at %s", wifi_band_mode_name(want), where);
	}
}
#endif /* CONFIG_IDF_TARGET_ESP32C5 */

void wifi_manager_init(void)
{
	esp_err_t err;
	uint8_t do_connect = 0;

	wifi_led_init();
	wifi_led_set_off();

	/* Mutex / event group first: credential memory and event handlers may run
	 * before the WiFi driver is fully configured. */
	ctx.lock = xSemaphoreCreateBinary();
	assert(ctx.lock);
	xSemaphoreGive(ctx.lock);
	ctx.events = xEventGroupCreate();
	assert(ctx.events);
	ctx.task = NULL;
	ctx.rotation_task = NULL;
	ctx.is_endless_connect = 0;
	ctx.auto_reconnect = 1;
	ctx.is_sta_connected = 0;
	ctx.rotation_cancel = 0;
	ctx.rotation_suppressed = 0;
	ctx.rotation_pending = 0;
	ctx.delayed_stopAP_task = NULL;
	ctx.delayed_startAP_task = NULL;
	ctx.try_connect_count = 0;
	ctx.pending_valid = false;
	ctx.pending_persist = false;
	/* Smart APSTA defaults (contract §5.6; tunable through WIFI cmd 7):
	 * AP off after STA connect, AP on 10 s after a disconnect. */
	ctx.ap_on_delay_tick = pdMS_TO_TICKS(10000);
#if defined(CONFIG_IDF_TARGET_ESP32C5)
	/* ESP32-C5 has a single dual-band Wi-Fi radio: keeping the fallback SoftAP
	 * alive while the STA is already associated makes large HTTP/OTA transfers
	 * stall (measured on real hardware 2026-10-25: the first Web page request
	 * after boot sent only ~6.7 KiB, then httpd reported "error in send : 11"
	 * (EAGAIN) after its 10 s send timeout; later requests succeeded once the
	 * AP had been stopped by the 30 s timer).  The AP is only a fallback for
	 * "no STA connection", so on the C5 stop it 2 s after the STA connects;
	 * it still comes back 10 s after a disconnect. */
	ctx.ap_off_delay_tick = pdMS_TO_TICKS(2000);
#else
	ctx.ap_off_delay_tick = pdMS_TO_TICKS(30000);
#endif

	/* Load/migrate the multi credential table before touching any STA/AP
	 * credential (docs/wifi-credential-memory.md §2/§7). */
	err = wifi_cred_mem_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "credential memory init: %s", esp_err_to_name(err));
	}

	ap_netif = esp_netif_create_default_wifi_ap();
	assert(ap_netif);
	sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	ESP_ERROR_CHECK(esp_netif_set_hostname(ap_netif, WIFI_DEFAULT_HOSTNAME));
	ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, WIFI_DEFAULT_HOSTNAME));

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

	ctx.permanent_mode = WIFI_AP_AUTO_STA_ON;
	err = wifi_data_get_wifi_mode(&ctx.permanent_mode);
	ESP_LOGI(TAG, "use wifi mode: %d", ctx.permanent_mode);
	if (err) {
		ctx.permanent_mode = WIFI_AP_AUTO_STA_ON;
		ctx.mode = WIFI_MODE_APSTA;
	} else {
		ctx.mode = ctx.permanent_mode & 0x3; /* 0b1XX */
		if (ctx.mode == WIFI_MODE_STA || ctx.mode == WIFI_MODE_NULL) {
			ctx.mode = WIFI_MODE_APSTA;
		}
	}

	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(ctx.mode));
	/* Pre-start: the driver stores the station power mode before it starts the
	 * RX path.  Non-fatal on error (logged); re-applied after start/connect. */
	wifi_manager_apply_no_power_save("init/set_mode");

	/* Effective AP credential: user value from NVS, else WiFi-Dap-XXXX
	 * derived from the STA MAC (contract §6). Never log the password. */
	if (wifi_manager_refresh_ap_credential()) {
		ESP_LOGE(TAG, "apply AP credential failed");
	}

	if (set_initial_sta_cred() == 0) {
		ESP_LOGI(TAG, "STA will connect to a saved credential");
		do_connect = 1;
	}

	wifi_api_sta_ap_static_info_t static_info;
	err = wifi_data_get_static(&static_info);
	if (err == ESP_OK) {
		wifi_manager_sta_set_static_conf(&static_info);
	}

	esp_netif_ip_info_t ip_info;
	IP4_ADDR_EXPAND(&ip_info.ip, WIFI_DEFAULT_AP_IP);
	IP4_ADDR_EXPAND(&ip_info.gw, WIFI_DEFAULT_AP_GATEWAY);
	IP4_ADDR_EXPAND(&ip_info.netmask, WIFI_DEFAULT_AP_NETMASK);

	err = esp_netif_dhcps_stop(ap_netif);
	if (err) {
		ESP_LOGE(TAG, "dhcps stop %s", esp_err_to_name(err));
	}
	err = esp_netif_set_ip_info(ap_netif, &ip_info);
	if (err) {
		ESP_LOGE(TAG, "net if set info %s", esp_err_to_name(err));
	}
	err = esp_netif_dhcps_start(ap_netif);
	if (err) {
		ESP_LOGE(TAG, "dhcps start %s", esp_err_to_name(err));
	}

	ESP_ERROR_CHECK(esp_wifi_start());
#if defined(CONFIG_IDF_TARGET_ESP32C5)
	/* The band-mode API is only accepted once the driver is started. */
	wifi_manager_apply_band_mode("esp_wifi_start");
#endif
	/* The pre-start power mode can be overridden by start/mode changes; force
	 * no power save again now that the driver is running (debug throughput). */
	wifi_manager_apply_no_power_save("esp_wifi_start");
	ESP_LOGI(TAG, "wifi started");
	esp_log_level_set("wifi", ESP_LOG_WARN);

	wifi_event_set_disco_handler(disconn_handler);
	if (do_connect) {
		disconn_handler();
	}

	ssdp_init();
	ssdp_start();
}

int wifi_manager_refresh_ap_credential(void)
{
	char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1] = {0};
	char password[BIRDIEBOT_WIFI_PASS_MAX + 1] = {0};
	wifi_credential_t cred = {0};
	size_t len;

	if (wifi_cred_mem_get_ap_cred(ssid, password) != ESP_OK) {
		return 1;
	}
	len = wifi_cred_strnlen(ssid, sizeof(ssid));
	if (len == 0 || len > sizeof(cred.ssid)) {
		return 1;
	}
	memcpy(cred.ssid, ssid, len);
	len = wifi_cred_strnlen(password, sizeof(password));
	if (len > sizeof(cred.password)) {
		len = sizeof(cred.password);
	}
	memcpy(cred.password, password, len);
	return wifi_manager_set_ap_credential(&cred);
}

static int set_initial_sta_cred(void)
{
	char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1] = {0};
	char password[BIRDIEBOT_WIFI_PASS_MAX + 1] = {0};

	if (wifi_cred_mem_initial_cred(ssid, password) != ESP_OK) {
		return -1;
	}
	if (set_sta_cred(ssid, password) != 0) {
		return -1;
	}
	rotation_prepare_pending(ssid, password, WIFI_API_CONNECT_SAVE_AUTO);
	ESP_LOGI(TAG, "initial STA credential: %s", ssid);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* LED                                                                       */
/* ------------------------------------------------------------------------- */

static void wifi_led_init(void)
{
#if BIRDIEBOT_BOARD_HAS_WIFI_LED
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << PIN_LED_WIFI_STATUS,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io);
	gpio_set_level(PIN_LED_WIFI_STATUS, 0);
#endif
}

static void wifi_led_set_state(bool on)
{
#if BIRDIEBOT_BOARD_HAS_WIFI_LED
	gpio_set_level(PIN_LED_WIFI_STATUS, on ? 1 : 0);
#else
	(void) on;
#endif
}

static void wifi_led_set_off(void)
{
	wifi_led_set_state(false);
}

static void wifi_led_set_on(void)
{
	wifi_led_set_state(true);
}

/* Called from the WiFi/IP event handler: LED on after STA got an IP,
 * off when the STA link is lost. Active level: drive HIGH = on (ZXR S3
 * PIN_LED_WIFI_STATUS = GPIO4). */
void wifi_manager_wifi_led_set(bool on)
{
	wifi_led_set_state(on);
	ESP_LOGI(TAG, "WiFi status LED %s", on ? "on" : "off");
}

/* ------------------------------------------------------------------------- */
/* scan                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief called by wifi_event_handler on scan done (one channel)
 * */
static void wifi_event_scan_channel_done(uint16_t number, wifi_ap_record_t *aps)
{
	(void)aps;
	ctx.scan_total_aps += number;
	if (ctx.task) {
		xTaskNotifyGive(ctx.task);
	}
}

/**
 * Run the per-channel scan (contract §5.3: <= 10 s, same rhythm as the C
 * implementation).
 *
 * F7 fix (docs/hardware-test-wifi.md §11.3): the multi-second per-channel waits
 * are executed WITHOUT ctx.lock. The lock is only taken to publish and clear the
 * scan context, so other WiFi commands (GET_SAVED_LIST/GET_AP_INFO/CONNECT/...)
 * are never blocked for the whole scan and a Web SCAN is not rejected with
 * code=6 just because the rotation task is scanning.
 *
 * @param cancellable true for the rotation task: abort as soon as a Web SCAN
 *                    requested rotation_cancel (<= one channel wait).
 * @return WIFI_SCAN_OK / WIFI_SCAN_FAILED / WIFI_SCAN_CANCELED / WIFI_SCAN_BUSY.
 */
static int scan_run(wifi_ap_record_t *aps, uint16_t max_ap, uint16_t *found,
                    bool cancellable)
{
	uint32_t ret;
	uint16_t number;
	int result = WIFI_SCAN_OK;

	if (aps == NULL || found == NULL || max_ap == 0) {
		return WIFI_SCAN_FAILED;
	}
	/* Publish the scan context under the lock, then release it for the waits. */
	if (xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGW(TAG, "scan setup: lock busy");
		return WIFI_SCAN_BUSY;
	}
	ctx.scan_ap = aps;
	ctx.scan_max_ap = (uint8_t)(max_ap > UINT8_MAX ? UINT8_MAX : max_ap);
	ctx.scan_total_aps = 0;
	ctx.task = xTaskGetCurrentTaskHandle();
	xSemaphoreGive(ctx.lock);

	for (int scan_channel = 1; scan_channel <= 13; ++scan_channel) {
		if (cancellable && ctx.rotation_cancel) {
			result = WIFI_SCAN_CANCELED;
			break;
		}
		if (ctx.scan_total_aps >= ctx.scan_max_ap) {
			break;
		}
		number = (uint16_t)(ctx.scan_max_ap - ctx.scan_total_aps);
		if (wifi_event_trigger_scan(scan_channel,
		                            wifi_event_scan_channel_done, number,
		                            &aps[ctx.scan_total_aps])) {
			ESP_LOGE(TAG, "trigger scan %d error", scan_channel);
			result = WIFI_SCAN_FAILED;
			break;
		}
		/* shadow wifi_event_scan_channel_done() called */
		vTaskDelay(pdMS_TO_TICKS(100));
		ret = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
		if (ret == 0) {
			/* timeout */
			ESP_LOGE(TAG, "scan channel %d timeout", scan_channel);
			result = WIFI_SCAN_FAILED;
			break;
		}
	}

	xSemaphoreTake(ctx.lock, portMAX_DELAY);
	*found = ctx.scan_total_aps;
	ctx.task = NULL;
	xSemaphoreGive(ctx.lock);
	return result;
}

int wifi_manager_get_scan_list(uint16_t *number, wifi_ap_record_t *aps)
{
	bool interrupted_rotation = false;
	uint16_t found = 0;
	int result;

	if (number == NULL || aps == NULL || *number == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	/* A Web scan interrupts an in-flight rotation (contract §5.7). The rotation
	 * task is stopped at a safe point instead of vTaskDelete() so no task ever
	 * dies while holding ctx.lock; its cancellable scan aborts within one
	 * channel wait. */
	ctx.rotation_suppressed = 1;
	ctx.rotation_pending = 0;
	if (ctx.rotation_task != NULL) {
		rotation_request_stop();
		interrupted_rotation = true;
	}
	if (ctx.rotation_task != NULL) {
		/* Rotation could not stop in time (radio stuck): tell the caller to
		 * retry instead of blocking/stealing the scan (JSON code=6, F7). */
		ESP_LOGW(TAG, "scan requested while rotation is still running");
		ctx.rotation_suppressed = 0;
		return ESP_ERR_NOT_FINISHED;
	}
	if (ctx.is_endless_connect) {
		ctx.is_endless_connect = 0;
	}
	if (interrupted_rotation) {
		/* Rotation may have been connecting with a stale credential. */
		ESP_LOGI(TAG, "Web scan interrupts credential rotation (F7 unlocked path)");
		esp_wifi_disconnect();
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	result = scan_run(aps, *number, &found, false);
	*number = found;

	/* Resume automatic reconnection after the Web scan finished. Also covers a
	 * disconnect that happened while the scan was running (disconn_handler()
	 * was suppressed during the scan). Do not start a fresh rotation when the
	 * STA was idle all along (AP only / no saved credentials). */
	ctx.rotation_suppressed = 0;
	if (ctx.auto_reconnect && ctx.is_sta_connected == 0 &&
	    (interrupted_rotation || ctx.rotation_pending)) {
		ctx.rotation_pending = 0;
		disconn_handler();
	}
	if (result == WIFI_SCAN_BUSY) {
		return ESP_ERR_NOT_FINISHED; /* code=6, retryable */
	}
	return (result == WIFI_SCAN_OK) ? ESP_OK : ESP_FAIL;
}

void *wifi_manager_get_ap_netif(void)
{
	return ap_netif;
}

void *wifi_manager_get_sta_netif(void)
{
	return sta_netif;
}

/* ------------------------------------------------------------------------- */
/* connect attempt plumbing                                                  */
/* ------------------------------------------------------------------------- */

static void try_connect_done(void *arg, ip_event_got_ip_t *event)
{
	(void)arg;
	ctx.conn_event = event;
	if (ctx.task) {
		xTaskNotifyGive(ctx.task);
	}
	xEventGroupSetBits(ctx.events, WIFI_EVT_CONNECT_DONE);
}

/**
 * Configure the STA with the given credential (bounded copy, no fixed size
 * memcpy from possibly shorter buffers -- docs §8.1 defect fix).
 */
static int set_sta_cred(const char *ssid, const char *password)
{
	wifi_config_t wifi_config = {0};
	size_t ssid_len;
	size_t password_len;
	int err;

	if (!wifi_cred_ssid_valid(ssid) || !wifi_cred_password_valid(password)) {
		ESP_LOGE(TAG, "invalid STA credential");
		return 1;
	}
	ssid_len = wifi_cred_strnlen(ssid, BIRDIEBOT_WIFI_SSID_MAX + 1);
	password_len = wifi_cred_strnlen(password, BIRDIEBOT_WIFI_PASS_MAX + 1);
	memcpy(wifi_config.sta.ssid, ssid, ssid_len);
	memcpy(wifi_config.sta.password, password, password_len);
	wifi_config.sta.threshold.rssi = -80;
	wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

	err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	if (err) {
		ESP_LOGE(TAG, "set sta credential: %s", esp_err_to_name(err));
		return 1;
	}
	return 0;
}

static void handle_wifi_connected(void)
{
	ctx.is_sta_connected = 1;
	/* A (re)association may reset the station power mode to the IDF default;
	 * restore WIFI_PS_NONE before any DAP traffic can use the link. */
	wifi_manager_apply_no_power_save("STA connected");
	wifi_led_set_on();

	if (ctx.pending_valid) {
		int err = wifi_cred_mem_note_success(ctx.pending_ssid, ctx.pending_password,
		                                     ctx.pending_persist);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "remember credential %s: %s", ctx.pending_ssid,
			         esp_err_to_name(err));
		}
		wifi_cred_mem_set_connected_ssid(ctx.pending_ssid);
	}

	if (ctx.delayed_startAP_task) {
		xTaskNotifyGive(ctx.delayed_startAP_task);
	}
	if (ctx.permanent_mode != WIFI_AP_AUTO_STA_ON) {
		return;
	}
	if (ctx.delayed_stopAP_task == NULL) {
		xTaskCreate(delayed_set_ap_stop, "stop ap", 4096,
		            NULL, tskIDLE_PRIORITY + 1, &ctx.delayed_stopAP_task);
	}
}

static void handle_wifi_disconnected(void)
{
	ctx.is_sta_connected = 0;
	wifi_cred_mem_set_connected_ssid(NULL);
	if (ctx.delayed_stopAP_task) {
		xTaskNotifyGive(ctx.delayed_stopAP_task);
	}
	if (ctx.permanent_mode != WIFI_AP_AUTO_STA_ON) {
		return;
	}
	++ctx.try_connect_count;
	if (ctx.delayed_startAP_task == NULL) {
		xTaskCreate(delayed_set_ap_start, "start ap", 4096,
		            NULL, tskIDLE_PRIORITY + 1, &ctx.delayed_startAP_task);
	}
}

/* ------------------------------------------------------------------------- */
/* rotation (contract §5)                                                    */
/* ------------------------------------------------------------------------- */

static bool ssid_in_visible_list(const char *ssid,
                                 const char (*visible)[BIRDIEBOT_WIFI_SSID_MAX + 1],
                                 int visible_count)
{
	for (int i = 0; i < visible_count; ++i) {
		if (strncmp(visible[i], ssid, BIRDIEBOT_WIFI_SSID_MAX + 1) == 0) {
			return true;
		}
	}
	return false;
}

/**
 * Scan the neighborhood and collect visible SSIDs (only used to intersect with
 * saved credentials; open networks are never auto-connected, contract §5.2).
 */
static int rotation_scan_visible(void)
{
	uint16_t found = 0;
	int count = 0;
	int result;

	if (ctx.rotation_cancel) {
		return 0;
	}
	/* Cancellable scan: a Web SCAN sets rotation_cancel and this aborts within
	 * one channel wait, WITHOUT holding ctx.lock for the scan (F7). */
	result = scan_run(s_rotation_scan_aps, WIFI_ROTATION_SCAN_MAX_AP, &found, true);
	if (result == WIFI_SCAN_CANCELED) {
		ESP_LOGD(TAG, "rotation scan canceled");
		return 0;
	}
	if (result != WIFI_SCAN_OK) {
		ESP_LOGW(TAG, "rotation scan failed (%d)", result);
		return 0;
	}

	for (uint16_t i = 0; i < found && count < WIFI_ROTATION_CANDIDATE_MAX; ++i) {
		uint8_t *raw_ssid = s_rotation_scan_aps[i].ssid;
		char ssid[BIRDIEBOT_WIFI_SSID_MAX + 1];
		bool duplicate = false;

		if (raw_ssid[0] == '\0' || s_rotation_scan_aps[i].rssi < -85) {
			continue;
		}
		memset(ssid, 0, sizeof(ssid));
		memcpy(ssid, raw_ssid, BIRDIEBOT_WIFI_SSID_MAX);
		if (!wifi_cred_ssid_valid(ssid)) {
			continue;
		}
		for (int j = 0; j < count; ++j) {
			if (strncmp(s_rotation_visible[j], ssid, sizeof(ssid)) == 0) {
				duplicate = true;
				break;
			}
		}
		if (duplicate) {
			continue;
		}
		wifi_cred_ssid_copy(s_rotation_visible[count], ssid);
		++count;
	}
	return count;
}

/**
 * Try one rotation candidate.
 * @return 1 connected, 0 failed, -1 canceled.
 */
static int rotation_execute_candidate(const wifi_cred_candidate_t *candidate)
{
	EventBits_t bits;
	uint32_t disco_count_before;
	int err;

	if (ctx.rotation_cancel) {
		return -1;
	}

	xSemaphoreTake(ctx.lock, portMAX_DELAY);
	if (ctx.rotation_cancel) {
		xSemaphoreGive(ctx.lock);
		return -1;
	}
	ctx.task = xTaskGetCurrentTaskHandle();
	/* esp_wifi_set_config is only meaningful for a disconnected station; the
	 * previous candidate may still be connecting (20 s timeout case). */
	esp_wifi_disconnect();
	vTaskDelay(pdMS_TO_TICKS(50));
	err = set_sta_cred(candidate->ssid, candidate->password);
	if (err == 0) {
		rotation_prepare_pending(candidate->ssid, candidate->password,
		                         WIFI_API_CONNECT_SAVE_AUTO);
		ctx.conn_event = NULL;
		xEventGroupClearBits(ctx.events, WIFI_EVT_CONNECT_DONE);
		disco_count_before = wifi_event_get_disconnect_count();
		err = wifi_event_trigger_connect(0, try_connect_done, NULL);
	} else {
		disco_count_before = 0;
	}
	xSemaphoreGive(ctx.lock);

	if (err) {
		ESP_LOGW(TAG, "trigger connect to \"%s\" failed", candidate->ssid);
		return 0;
	}

	bits = xEventGroupWaitBits(ctx.events, WIFI_EVT_CONNECT_DONE | WIFI_EVT_CANCEL,
	                           pdTRUE, pdFALSE,
	                           pdMS_TO_TICKS(WIFI_CONNECT_ATTEMPT_TIMEOUT_MS));
	if (bits & WIFI_EVT_CANCEL) {
		return -1;
	}
	if (ctx.conn_event != NULL) {
		ctx.conn_event = NULL;
		return 1;
	}

	/* Failed. If the AP rejected the credential (auth / 4-way handshake), skip
	 * this entry for the rest of the power cycle (contract §4.5). */
	if (wifi_event_get_disconnect_count() != disco_count_before) {
		int reason = wifi_event_get_last_disconnect_reason();
		if (disconnect_reason_is_credential(reason)) {
			ESP_LOGW(TAG, "credential \"%s\" rejected (reason %d), skip this boot",
			         candidate->ssid, reason);
			wifi_cred_mem_note_connect_failure(candidate->ssid, true);
		}
	}
	return 0;
}

/*
 * Try every candidate in rotation order.
 * @param visible NULL => step 2 (no scan), else step 3 (visible ∩ saved).
 * @return 1 connected, 0 no success, -1 canceled.
 */
static int rotation_run_candidates(const char (*visible)[BIRDIEBOT_WIFI_SSID_MAX + 1],
                                   int visible_count)
{
	int planned = wifi_cred_mem_rotation_plan(s_rotation_candidates,
	                                          WIFI_ROTATION_CANDIDATE_MAX);

	for (int i = 0; i < planned; ++i) {
		int ret;

		if (ctx.rotation_cancel || ctx.auto_reconnect == 0) {
			return -1;
		}
		if (visible != NULL &&
		    !ssid_in_visible_list(s_rotation_candidates[i].ssid, visible, visible_count)) {
			continue;
		}
		while (wifi_cred_mem_rotation_paused() && ctx.rotation_cancel == 0) {
			EventBits_t bits = xEventGroupWaitBits(ctx.events, WIFI_EVT_CANCEL,
			                                       pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
			if (bits & WIFI_EVT_CANCEL) {
				return -1;
			}
		}
		ESP_LOGI(TAG, "rotation: try \"%s\" (priority %u, last_success %lu)",
		         s_rotation_candidates[i].ssid, s_rotation_candidates[i].priority,
		         (unsigned long)s_rotation_candidates[i].last_success_ts);
		ret = rotation_execute_candidate(&s_rotation_candidates[i]);
		if (ret == 1) {
			return 1;
		}
		if (ret < 0) {
			return -1;
		}
	}
	return 0;
}

/* Single rotation task: last success -> other saved -> visible saved -> backoff */
static void rotation_task(void *arg)
{
	(void)arg;
	uint8_t backoff_step = 0;

	ctx.is_endless_connect = 1;
	ctx.rotation_task = xTaskGetCurrentTaskHandle();
	/* Do NOT clear ctx.rotation_cancel here: a Web SCAN may have requested the
	 * stop between xTaskCreate() and this first instruction. disconn_handler()
	 * already cleared it before creating the task. */
	/* disconn_handler() took the lock before creating this task. */
	xSemaphoreGive(ctx.lock);

	while (ctx.rotation_cancel == 0 && ctx.auto_reconnect) {
		int ret = rotation_run_candidates(NULL, 0);

		if (ret == 1) {
			handle_wifi_connected();
			break;
		}
		if (ret < 0) {
			break;
		}

		/* step 3: scan, then retry visible saved networks in the same order */
		int visible_count = rotation_scan_visible();
		if (ctx.rotation_cancel) {
			break;
		}
		if (visible_count > 0) {
			ret = rotation_run_candidates((const char (*)[BIRDIEBOT_WIFI_SSID_MAX + 1])s_rotation_visible,
			                              visible_count);
			if (ret == 1) {
				handle_wifi_connected();
				break;
			}
			if (ret < 0) {
				break;
			}
		}

		while (wifi_cred_mem_rotation_paused() && ctx.rotation_cancel == 0) {
			EventBits_t bits = xEventGroupWaitBits(ctx.events, WIFI_EVT_CANCEL,
			                                       pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
			if (bits & WIFI_EVT_CANCEL) {
				break;
			}
		}
		if (ctx.rotation_cancel) {
			break;
		}

		/* step 4: 5/10/20/30 s backoff, capped at 30 s (contract §5.1) */
		{
			uint32_t wait_ms = s_rotation_backoff_ms[backoff_step];
			size_t max_step = sizeof(s_rotation_backoff_ms) / sizeof(s_rotation_backoff_ms[0]) - 1;
			EventBits_t bits;

			if (backoff_step < max_step) {
				++backoff_step;
			}
			ESP_LOGI(TAG, "rotation: no connectable saved network, retry in %lu ms",
			         (unsigned long)wait_ms);
			bits = xEventGroupWaitBits(ctx.events, WIFI_EVT_CANCEL | WIFI_EVT_RESTART,
			                           pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
			if (bits & WIFI_EVT_CANCEL) {
				break;
			}
			if (bits & WIFI_EVT_RESTART) {
				backoff_step = 0; /* fresh disconnect: start from step 1 again */
			}
		}
	}

	{
		bool need_restart = (ctx.auto_reconnect && ctx.rotation_cancel == 0 &&
		                     ctx.rotation_suppressed == 0 && ctx.is_sta_connected == 0);

		ctx.is_endless_connect = 0;
		ctx.task = NULL;
		ctx.rotation_task = NULL;
		ctx.rotation_cancel = 0;
		if (need_restart) {
			/* A disconnect raced with our exit: start a new rotation task. */
			disconn_handler();
		}
	}
	vTaskDelete(NULL);
}

static void rotation_request_stop(void)
{
	if (ctx.rotation_task == NULL) {
		return;
	}
	ctx.rotation_cancel = 1;
	xEventGroupSetBits(ctx.events, WIFI_EVT_CANCEL);
	/* Rotation holds no lock while waiting, so it stops promptly; allow a
	 * full scan window (<= 10 s) at worst. */
	for (int i = 0; i < 1000 && ctx.rotation_task != NULL; ++i) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	if (ctx.rotation_task != NULL) {
		ESP_LOGW(TAG, "rotation task did not stop in time");
	}
}

static void disconn_handler(void)
{
	/* disconnected
	 * 1. WI-FI AP is far away
	 * 2. WI-FI AP or AP device is closed
	 * 3. this device is ejected by AP
	 * */
	wifi_led_set_off();
	handle_wifi_disconnected();
	if (ctx.auto_reconnect == 0) {
		return;
	}
	if (ctx.rotation_suppressed) {
		/* A scan/manual connect owns the radio right now; the caller re-arms
		 * the rotation when it is done. */
		ctx.rotation_pending = 1;
		return;
	}
	if (ctx.rotation_task != NULL) {
		/* already rotating: restart the cycle immediately */
		xEventGroupSetBits(ctx.events, WIFI_EVT_RESTART);
		return;
	}

	if (xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		ESP_LOGE(TAG, "disconn_handler: took no lock");
		return;
	}
	ctx.rotation_cancel = 0;
	ctx.is_endless_connect = 1;
	ctx.rotation_pending = 0;
	if (xTaskCreate(rotation_task, "reconn task", 5 * 1024, NULL, 7,
	                &ctx.rotation_task) != pdPASS) {
		ctx.is_endless_connect = 0;
		ctx.rotation_task = NULL;
		xSemaphoreGive(ctx.lock);
		ESP_LOGE(TAG, "cannot create rotation task");
	}
}

/* ------------------------------------------------------------------------- */
/* public connect / disconnect                                               */
/* ------------------------------------------------------------------------- */

int wifi_manager_connect(const char *ssid, const char *password, int save)
{
	EventBits_t bits;
	int ret = 1;
	int err;

	if (!wifi_cred_ssid_valid(ssid) || !wifi_cred_password_valid(password)) {
		return 1;
	}
	if (save != WIFI_API_CONNECT_SAVE_AUTO && save != WIFI_API_CONNECT_SAVE_NO &&
	    save != WIFI_API_CONNECT_SAVE_YES) {
		return 1;
	}

	/* Manual CONNECT cancels the current rotation and retries even credentials
	 * that failed earlier this boot (contract §4.5/§5.4). */
	ctx.rotation_suppressed = 1;
	rotation_request_stop();
	if (xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		ctx.rotation_suppressed = 0;
		return 1;
	}

	ctx.auto_reconnect = 1;
	wifi_cred_mem_clear_failures();
	esp_wifi_disconnect();
	vTaskDelay(pdMS_TO_TICKS(100));
	rotation_prepare_pending(ssid, password, save);

	if (set_sta_cred(ssid, password) == 0) {
		ctx.task = xTaskGetCurrentTaskHandle();
		ctx.conn_event = NULL;
		xEventGroupClearBits(ctx.events, WIFI_EVT_CONNECT_DONE);
		err = wifi_event_trigger_connect(2, try_connect_done, NULL);
		if (err == 0) {
			bits = xEventGroupWaitBits(ctx.events, WIFI_EVT_CONNECT_DONE,
			                           pdTRUE, pdFALSE,
			                           pdMS_TO_TICKS(WIFI_MANUAL_CONNECT_TIMEOUT_MS));
			ret = ((bits & WIFI_EVT_CONNECT_DONE) && ctx.conn_event != NULL) ? 0 : 1;
		}
		ctx.task = NULL;
		ctx.conn_event = NULL;
	}
	xSemaphoreGive(ctx.lock);
	ctx.rotation_suppressed = 0;
	ctx.rotation_pending = 0;

	if (ret != 0) {
		ESP_LOGW(TAG, "connect to \"%s\" failed, resuming rotation", ssid);
		ctx.is_sta_connected = 0;
		if (ctx.auto_reconnect) {
			disconn_handler();
		}
		return 1;
	}
	handle_wifi_connected();
	return 0;
}

int wifi_manager_disconnect(void)
{
	int err;

	ctx.auto_reconnect = 0;
	rotation_request_stop();
	ctx.rotation_suppressed = 0;
	err = esp_wifi_disconnect();
	if (err) {
		/* Disconnecting an already disconnected station is not an error for
		 * the JSON cmd 4 (contract: stop auto reconnect, AP stays up). */
		ESP_LOGD(TAG, "disconnect: %s", esp_err_to_name(err));
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* AP/STA mode + Smart APSTA timers (unchanged behaviour)                    */
/* ------------------------------------------------------------------------- */

int wifi_manager_change_mode(wifi_apsta_mode_e mode)
{
	int err;
	if (ctx.permanent_mode == mode) {
		return 0;
	}
	err = xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(2000));
	if (err != pdTRUE) {
		ESP_LOGE(TAG, "set mode take lock");
		return 1;
	}
	if (ctx.delayed_stopAP_task) {
		xTaskNotifyGive(ctx.delayed_stopAP_task);
	}
	if (ctx.delayed_startAP_task) {
		xTaskNotifyGive(ctx.delayed_startAP_task);
	}

	err = set_wifi_mode(mode);
	xSemaphoreGive(ctx.lock);
	if (err)
		return err;
	if (mode >= WIFI_AP_STOP){
		return ESP_OK;
	}
	return wifi_data_save_wifi_mode(mode);
}

static int set_wifi_mode(wifi_apsta_mode_e mode)
{
	uint32_t new_mode = ctx.mode;

	switch (mode) {
	default:
		return 1;
	case WIFI_AP_AUTO_STA_ON:
		if (ctx.is_sta_connected) {
			new_mode = WIFI_MODE_STA;
		} else {
			new_mode = WIFI_MODE_APSTA;
		}
		ctx.permanent_mode = mode;
		break;
	case WIFI_AP_STA_OFF:
	case WIFI_AP_ON_STA_OFF:
	case WIFI_AP_OFF_STA_ON:
	case WIFI_AP_STA_ON:
		ctx.permanent_mode = mode;
		new_mode = mode & (~WIFI_AP_STA_OFF);
		break;

	case WIFI_AP_STOP:
		new_mode &= ~WIFI_MODE_AP;
		break;
	case WIFI_AP_START:
		new_mode |= WIFI_MODE_AP;
		break;
	case WIFI_STA_STOP:
		new_mode &= ~WIFI_MODE_STA;
		break;
	case WIFI_STA_START:
		new_mode |= WIFI_MODE_STA;
		break;
	}

	if (new_mode == ctx.mode) {
		return 0;
	}

	int err = esp_wifi_set_mode(new_mode);
	if (err) {
		ESP_LOGE(TAG, "set mode %lx: %s", (unsigned long)new_mode, esp_err_to_name(err));
		return err;
	}

	/* AP starts later than boot (cmd 7): apply the stored effective AP
	 * credential now, the driver does not read NVS on its own. */
	if ((new_mode & WIFI_MODE_AP) && !(ctx.mode & WIFI_MODE_AP)) {
		wifi_manager_refresh_ap_credential();
	}

	/* AP -> APSTA: (re)start the saved STA connection. */
	if (!(ctx.mode & WIFI_MODE_STA) && (new_mode & WIFI_MODE_STA)) {
		xSemaphoreGive(ctx.lock);
		disconn_handler();
	}
	ctx.mode = new_mode;
	/* esp_wifi_set_mode() can reset the station power mode; re-apply while the
	 * lock is still held (the helper only calls the Wi-Fi driver). */
	wifi_manager_apply_no_power_save("esp_wifi_set_mode");
#if defined(CONFIG_IDF_TARGET_ESP32C5)
	/* Keep the C5 band mode across AP/STA mode transitions as well. */
	wifi_manager_apply_band_mode("esp_wifi_set_mode");
#endif
	return err;
}

static void delayed_set_ap_stop(void *arg)
{
	uint32_t ret = ulTaskNotifyTake(pdTRUE, ctx.ap_off_delay_tick);
	if (xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
		ctx.delayed_stopAP_task = NULL;
		vTaskDelete(NULL);
		return;
	}
	/* timeout: connected to STA without disconnection: close AP */
	if (ret == 0) {
		set_wifi_mode(WIFI_AP_STOP);
		ctx.try_connect_count = 0;
	}

	ctx.delayed_stopAP_task = NULL;
	xSemaphoreGive(ctx.lock);
	vTaskDelete(NULL);
}

static void delayed_set_ap_start(void *arg)
{
	uint32_t ret = 0;
	(void)arg;

	if (!(ctx.try_connect_count > 5)) {
		ret = ulTaskNotifyTake(pdTRUE, ctx.ap_on_delay_tick);
	}
	if (xSemaphoreTake(ctx.lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
		ctx.delayed_startAP_task = NULL;
		vTaskDelete(NULL);
		return;
	}
	/* timeout: connected to STA without disconnection for 10 sec: close AP
	 * or consecutive connect/disconnect */
	if (ret == 0 && ctx.permanent_mode == WIFI_AP_AUTO_STA_ON) {
		set_wifi_mode(WIFI_AP_START);
		ctx.try_connect_count = 0;
	}

	ctx.delayed_startAP_task = NULL;
	xSemaphoreGive(ctx.lock);
	vTaskDelete(NULL);
}

int wifi_manager_get_ap_auto_delay(int *ap_on_delay, int *ap_off_delay)
{
	if (ap_on_delay == NULL || ap_off_delay == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	*ap_on_delay = pdTICKS_TO_MS(ctx.ap_on_delay_tick);
	*ap_off_delay = pdTICKS_TO_MS(ctx.ap_off_delay_tick);
	return 0;
}

int wifi_manager_set_ap_auto_delay(int *ap_on_delay, int *ap_off_delay)
{
	if (ap_on_delay == NULL || ap_off_delay == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	/* Fix (docs §8.1): the original code overwrote ap_on_delay_tick twice and
	 * ap_off_delay_tick never took effect. */
	ctx.ap_on_delay_tick = pdMS_TO_TICKS(*ap_on_delay);
	ctx.ap_off_delay_tick = pdMS_TO_TICKS(*ap_off_delay);
	return wifi_manager_get_ap_auto_delay(ap_on_delay, ap_off_delay);
}

int wifi_manager_get_mode(wifi_apsta_mode_e *mode, wifi_mode_t *status)
{
	if (mode == NULL || status == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	*mode = ctx.permanent_mode;
	*status = ctx.mode;
	return 0;
}

int wifi_manager_set_ap_credential(wifi_credential_t *cred)
{
	wifi_config_t ap_config = {0};
	wifi_mode_t mode;
	size_t ssid_len;
	size_t password_len;
	int err;

	if (cred == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	ssid_len = wifi_cred_strnlen(cred->ssid, sizeof(cred->ssid));
	password_len = wifi_cred_strnlen(cred->password, sizeof(cred->password));
	if (ssid_len == 0 || ssid_len > BIRDIEBOT_WIFI_SSID_MAX) {
		return ESP_ERR_INVALID_ARG;
	}
	if (password_len > BIRDIEBOT_WIFI_AP_PASS_MAX) {
		password_len = BIRDIEBOT_WIFI_AP_PASS_MAX;
	}
	memcpy(ap_config.ap.ssid, cred->ssid, ssid_len);
	ap_config.ap.ssid_len = ssid_len;
	memcpy(ap_config.ap.password, cred->password, password_len);
	ap_config.ap.authmode = (password_len >= BIRDIEBOT_WIFI_AP_PASS_MIN)
	                        ? WIFI_AUTH_WPA2_WPA3_PSK : WIFI_AUTH_OPEN;
	ap_config.ap.max_connection = 4;
	ap_config.ap.channel = 6;
	ap_config.ap.ssid_hidden = 0;

	err = esp_wifi_get_mode(&mode);
	if (err) {
		return err;
	}
	if (mode & WIFI_MODE_AP) {
		err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
		if (err) {
			ESP_LOGE(TAG, "ap conf %s", esp_err_to_name(err));
		}
	}

	return 0;
}

int wifi_manager_sta_set_static_conf(wifi_api_sta_ap_static_info_t *static_info)
{
	if (static_info == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (static_info->static_ip_en) {
		esp_netif_ip_info_t ip_info;
		ip_info.ip.addr = static_info->ip.addr;
		ip_info.gw.addr = static_info->gateway.addr;
		ip_info.netmask.addr = static_info->netmask.addr;
		esp_netif_dhcpc_stop(wifi_manager_get_sta_netif());
		esp_netif_set_ip_info(wifi_manager_get_sta_netif(), &ip_info);
	} else {
		esp_netif_dhcpc_start(wifi_manager_get_sta_netif());
	}

	if (static_info->static_dns_en) {
		esp_netif_dns_info_t dns_info = {0};
		dns_info.ip.type = ESP_IPADDR_TYPE_V4;
		dns_info.ip.u_addr.ip4.addr = static_info->dns_main.addr;
		esp_netif_set_dns_info(wifi_manager_get_sta_netif(), ESP_NETIF_DNS_MAIN, &dns_info);
		dns_info.ip.u_addr.ip4.addr = static_info->dns_backup.addr;
		esp_netif_set_dns_info(wifi_manager_get_sta_netif(), ESP_NETIF_DNS_BACKUP, &dns_info);
	} else {
		/* Auto DNS: use the gateway as DNS server. The old code assigned an
		 * enum value to dns_info.ip.type here (docs §8.1 defect). */
		esp_netif_ip_info_t ip_info;
		esp_netif_dns_info_t dns_info = {0};
		dns_info.ip.type = ESP_IPADDR_TYPE_V4;
		esp_netif_get_ip_info(wifi_manager_get_sta_netif(), &ip_info);
		dns_info.ip.u_addr.ip4.addr = ip_info.gw.addr;
		esp_netif_set_dns_info(wifi_manager_get_sta_netif(), ESP_NETIF_DNS_MAIN, &dns_info);
		esp_netif_set_dns_info(wifi_manager_get_sta_netif(), ESP_NETIF_DNS_BACKUP, &dns_info);
	}
	return 0;
}
