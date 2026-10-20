#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
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

#include <sdkconfig.h>

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

void app_main()
{
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

    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 14, NULL);

    // DAP handle task
    xTaskCreate(DAP_Thread, "DAP_Task", 2048, NULL, 10, NULL);

	xTaskCreate(uart_bridge_task, "uart_server", 4096, NULL, 2, NULL);

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
	/* New image after OTA: confirm within 10 s or the next reset rolls back. */
	ota_confirm_schedule();
#endif
}
