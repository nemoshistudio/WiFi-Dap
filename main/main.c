#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_netif.h>

#include "tcp_server.h"
#include "cmsis-dap/include/DAP.h"
#include "DAP_handle.h"
#include "wt_mdns_config.h"
#include "wt_storage.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "memory_pool.h"
#include "request_runner.h"
#include "uart_tcp_bridge.h"
#include "global_module.h"
#include "wt_ota.h"
#include "project_components/wt_common/wt_protocol_version.h"
#include "components/board/include/birdiebot_board.h"

#include <sdkconfig.h>

#if defined(CONFIG_BIRDIEBOT_BOARD_ZXR_DAP_S3)
#define BIRDIEBOT_BOARD_STR "BOARD_ZXR_DAP_S3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_S3)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_S3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_C3)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_C3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_ESP32)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_ESP32"
#else
#define BIRDIEBOT_BOARD_STR "BOARD_UNKNOWN"
#endif

#include <assert.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

/*
 * OTA rollback confirmation (docs/ota-local-upload.md §2.3, WP-205).
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y an image installed through the
 * browser-upload OTA flow boots in ESP_OTA_IMG_PENDING_VERIFY. The running
 * application must confirm it after basic services are up, otherwise the next
 * reset makes the bootloader roll back to the previous slot. A single 10 s
 * esp_timer is registered at the end of app_main(), after WiFi, the webserver
 * and the DAP/UART tasks have been started; the callback calls
 * wt_ota_mark_app_valid(), which only acts when the image is pending verify.
 *
 * When CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID=y the wt_ota component schedules
 * the same confirmation itself, so the main hook is compiled out.
 */
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
#define BIRDIEBOT_OTA_CONFIRM_DELAY_US (10 * 1000 * 1000)

static void ota_confirm_timer_cb(void *arg)
{
	(void)arg;
	esp_err_t err = wt_ota_mark_app_valid();
	if (err == ESP_OK) {
		ESP_LOGI("APP", "OTA running image confirmed (rollback cancelled)");
	} else {
		ESP_LOGW("APP", "OTA running image confirmation failed: %s", esp_err_to_name(err));
	}
}

static void ota_confirm_schedule(void)
{
	static esp_timer_handle_t confirm_timer = NULL;

	const esp_timer_create_args_t timer_args = {
		.callback = &ota_confirm_timer_cb,
		.arg = NULL,
		.name = "ota_confirm",
		.dispatch_method = ESP_TIMER_TASK,
	};

	esp_err_t err = esp_timer_create(&timer_args, &confirm_timer);
	if (err != ESP_OK) {
		ESP_LOGE("APP", "cannot create OTA confirm timer: %s", esp_err_to_name(err));
		return;
	}
	err = esp_timer_start_once(confirm_timer, BIRDIEBOT_OTA_CONFIRM_DELAY_US);
	if (err != ESP_OK) {
		ESP_LOGE("APP", "cannot start OTA confirm timer: %s", esp_err_to_name(err));
		esp_timer_delete(confirm_timer);
		confirm_timer = NULL;
		return;
	}
	ESP_LOGI("APP", "OTA rollback confirmation scheduled in 10s");
}
#endif /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE && !CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID */

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define DAP_TASK_AFFINITY 1 /* dedicated GPIO bit-banging must stay on core 1 */
#else
#define DAP_TASK_AFFINITY 0
#endif

void app_main()
{
	/* docs/versioning.md §2.4: first log line, before WiFi/mDNS can flood it. */
	{
		const esp_app_desc_t *app = esp_app_get_description();
		ESP_LOGI("BIRDIEBOT", "WiFi Dap fw=%s proto=%d board=%s idf=%s",
		         app ? app->version : "unknown",
		         BIRDIEBOT_PROTOCOL_VERSION,
		         BIRDIEBOT_BOARD_STR,
		         esp_get_idf_version());
	}

	assert(memory_pool_init() == 0); // static buffer
	assert(request_runner_init() == 0);
	wt_storage_init();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wt_mdns_init();

	wifi_manager_init();
    DAP_Setup();

	global_module_init();

	start_webserver();

    /*
     * A3 task tuning (ESP32-S3):
     *  - tcp_server (USBIP/Wi-Fi front-end) stays at prio 14 on core 1;
     *  - the DAP task now runs at prio 16 (higher than tcp_server) so that once
     *    an OUT URB is queued, the DAP command is executed before lwIP/Wi-Fi
     *    processing can preempt it: this cuts the jitter of the USBIP
     *    request/response round-trip (the dominant cost for OpenOCD reads);
     *  - both stay pinned to core 1 because the SWD/JTAG bit-banging uses
     *    dedicated GPIO with a hard core affinity requirement.
     */
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 4096, NULL, 14, NULL, DAP_TASK_AFFINITY);
    /* 3072: with B1 (BIRDIEBOT_USBIP_PIPELINE) the DAP task also builds/sends
     * the USBIP RET_SUBMIT for a parked IN URB, so it needs lwIP send stack. */
    xTaskCreatePinnedToCore(DAP_Thread, "DAP_Task", 3072, NULL, 16, NULL, DAP_TASK_AFFINITY);

	xTaskCreate(uart_bridge_task, "uart_server", 4096, NULL, 2, NULL);

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
	/* New image after OTA: confirm within 10 s or the next reset rolls back. */
	ota_confirm_schedule();
#endif
}
