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
#include "birdiebot_probe_identity.h"
#include "dap_tcp_server.h"
#include "dap_tcp_bridge.h"

#include <sdkconfig.h>

/* C1 wired DAP is a build variant whose component (plus its esp_tinyusb
 * dependency) only exists for ESP32-S3 targets: the component manager excludes
 * it from ESP32-C3/ESP32 builds, so its header must not be included
 * unconditionally or those default builds fail.  It is also compiled out of the
 * default S3 build (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NONE). */
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#include "birdiebot_wired_dap.h"
#endif

#if defined(CONFIG_BIRDIEBOT_BOARD_WIFI_DAP_S3)
#define BIRDIEBOT_BOARD_STR "BOARD_WIFI_DAP_S3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_S3)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_S3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_C3)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_C3"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_ESP32)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_ESP32"
#elif defined(CONFIG_BIRDIEBOT_BOARD_GENERIC_C5)
#define BIRDIEBOT_BOARD_STR "BOARD_GENERIC_C5"
#else
#define BIRDIEBOT_BOARD_STR "BOARD_UNKNOWN"
#endif

#include <assert.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps (C5 PSRAM task stacks) */
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

/* The SWD bit-banging uses per-core dedicated-GPIO signals (S3: CORE1 GPIO,
 * C3/C5: RISC-V CPU GPIO CSRs), so the DAP core must be 1 on dual-core S3 and
 * 0 on the single-core targets. The ESP32 has no dedicated GPIO fast path and
 * has always run the DAP task on core 0. */
#if defined(CONFIG_IDF_TARGET_ESP32S3) && (portNUM_PROCESSORS > 1)
#define DAP_TASK_AFFINITY 1 /* dedicated GPIO bit-banging must stay on core 1 */
#else
#define DAP_TASK_AFFINITY 0
#endif

/* B1 task-tuning knobs (components/dap_proxy/Kconfig).  The defaults keep the
 * pre-B1 placement/priorities; the values are A/B measured in the hardware
 * window.  Guarded so existing sdkconfig files without the new options still
 * build. */
#ifndef CONFIG_BIRDIEBOT_USBIP_TCP_PRIORITY
#define CONFIG_BIRDIEBOT_USBIP_TCP_PRIORITY 14
#endif
#ifndef CONFIG_BIRDIEBOT_DAP_TASK_PRIORITY
#define CONFIG_BIRDIEBOT_DAP_TASK_PRIORITY 16
#endif
#if defined(CONFIG_BIRDIEBOT_USBIP_TCP_CORE) && (CONFIG_BIRDIEBOT_USBIP_TCP_CORE != 1)
#define TCP_TASK_AFFINITY CONFIG_BIRDIEBOT_USBIP_TCP_CORE
#elif (portNUM_PROCESSORS > 1)
#define TCP_TASK_AFFINITY DAP_TASK_AFFINITY
#else
/* Single-core target (ESP32-C5): never pin above the only core, Kconfig
 * default 1 would trip configASSERT(taskVALID_CORE_ID()) in FreeRTOS. */
#define TCP_TASK_AFFINITY 0
#endif

/* Internal-RAM budget: the single-core ESP32-C5 heap is much smaller than the
 * S3's, so keep the USBIP front-end task stack at the measured minimum there
 * (it is a network-only task; the DAP bit-banging stack is separate). */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define BIRDIEBOT_TCP_TASK_STACK 3072
#else
#define BIRDIEBOT_TCP_TASK_STACK 4096
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

	/* Runtime probe identity: unique serial (DAP_Info / USB descriptor / mDNS
	 * TXT) derived from the factory WiFi STA MAC.  Kept after the first log
	 * line so docs/versioning.md §2.4 ("first log line") stays true. */
	birdiebot_probe_identity_init();

	/* PSRAM bring-up diagnostics (C5/S3 PSRAM builds): one line at boot, no
	 * per-request logging.  heap_largest stays internal-only so the DMA /
	 * WiFi / lwIP budget is visible after the PSRAM migrations. */
#if defined(CONFIG_SPIRAM) && (defined(CONFIG_IDF_TARGET_ESP32C5) || \
                                defined(CONFIG_IDF_TARGET_ESP32S3))
	ESP_LOGI("BIRDIEBOT",
	         "PSRAM ready: total_free=%u internal_free=%u internal_largest=%u "
	         "psram_free=%u psram_largest=%u",
	         (unsigned)esp_get_free_heap_size(),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
	                                                    MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif

	assert(memory_pool_init() == 0); // static buffer
	assert(request_runner_init() == 0);
	wt_storage_init();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM) && defined(CONFIG_IDF_TARGET_ESP32S3)
	/* C1 wired build: bring up the USB-NCM netif before network services start.
	 * Default (CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NONE) builds compile this out. */
	{
		esp_err_t wired_err = birdiebot_wired_dap_init();
		if (wired_err != ESP_OK) {
			ESP_LOGW("BIRDIEBOT", "wired DAP (USB-NCM) init failed: %s; continuing Wi-Fi only",
			         esp_err_to_name(wired_err));
		}
	}
#endif

	/*
	 * P0-2 DAP ring storage reservation: DEFAULT OFF.
	 *
	 * Real-device regression (2026-10-25): reserving the two ~41 KiB ring
	 * storages up-front made the unified NCM+WiFi image lose the USB-NCM
	 * device (303a:4000 stopped enumerating) and left the internal heap at
	 * ~17 KiB free / 4.6 KiB largest (Dashboard: ~7 KiB / 1.6 KiB).  The
	 * known-good lazy xRingbufferCreate() path in DAP_Thread is therefore the
	 * default again (BIRDIEBOT_DAP_RING_PREALLOC=0, see DAP_handle.h).  The
	 * reservation implementation is kept and can be re-enabled by building
	 * with -DBIRDIEBOT_DAP_RING_PREALLOC=1.
	 *
	 * The DAP_Thread ring (re-)create retry loop remains the fragmentation
	 * mitigation for target/curve: it allocates only when the DAP task starts
	 * (i.e. never from the early boot heap) and retries a transient failure
	 * instead of killing the DAP task.
	 */
#if BIRDIEBOT_DAP_RING_PREALLOC
	{
		size_t free_before = esp_get_free_heap_size();
		size_t largest_before =
			heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
		esp_err_t reserve_err = dap_handle_prealloc_ringbuffers();

		ESP_LOGI("BIRDIEBOT",
		         "heap before DAP ring reserve: free=%u largest=%u -> reserve %s "
		         "(free=%u largest=%u)",
		         (unsigned)free_before, (unsigned)largest_before,
		         reserve_err == ESP_OK ? "OK" : esp_err_to_name(reserve_err),
		         (unsigned)esp_get_free_heap_size(),
		         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
		                                                    MALLOC_CAP_INTERNAL));
	}
#endif

	wt_mdns_init();

	wifi_manager_init();
	/* P0-2 diagnostics: heap state after NCM/WiFi stack initialization.  The
	 * single largest block is what the JSON curve body / OTA buffers need, so
	 * log it here (also visible through GET /api/target/status). */
	ESP_LOGI("BIRDIEBOT", "heap after network init: free=%u largest=%u",
	         (unsigned)esp_get_free_heap_size(),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
	                                                    MALLOC_CAP_INTERNAL));
    DAP_Setup();

	/* Bring-up memory accounting (single-core C5 has a tight internal heap):
	 * the stage that eats the internal RAM must be visible, because a later
	 * xTaskCreate/ring allocation failure is silent otherwise. */
#if defined(CONFIG_SPIRAM) && (defined(CONFIG_IDF_TARGET_ESP32C5) || \
                                defined(CONFIG_IDF_TARGET_ESP32S3))
#define BIRDIEBOT_LOG_INTERNAL_HEAP(stage)                                   \
	ESP_LOGI("BIRDIEBOT", "internal heap %-18s free=%u largest=%u",         \
	         stage,                                                          \
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |         \
	                                           MALLOC_CAP_8BIT),             \
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |\
	                                                    MALLOC_CAP_8BIT))
	BIRDIEBOT_LOG_INTERNAL_HEAP("after DAP_Setup");
#else
#define BIRDIEBOT_LOG_INTERNAL_HEAP(stage) ((void)0)
#endif

	global_module_init();
	BIRDIEBOT_LOG_INTERNAL_HEAP("after modules");

	start_webserver();
	BIRDIEBOT_LOG_INTERNAL_HEAP("after webserver");

	/* M3: CMSIS-DAP over TCP backend (OpenOCD `cmsis-dap backend tcp`, port
	 * 3241).  Kconfig default off; when off both calls are safe no-ops.
	 * dap_tcp_bridge_init() makes main.o reference the strong DAP executor
	 * hooks in main/dap_tcp_bridge.c (otherwise the linker would keep the weak
	 * stubs).  Ownership hand-off (DELETE_HANDLE/RESET_HANDLE) is in that file;
	 * see docs/dap-tcp-backend.md §7.2. */
	dap_tcp_bridge_init();
	{
		const esp_err_t dap_tcp_err = dap_tcp_server_start();
		if ((dap_tcp_err != ESP_OK) &&
		    (dap_tcp_err != ESP_ERR_NOT_SUPPORTED)) {
			ESP_LOGW("BIRDIEBOT",
			         "DAP TCP backend not started: %s",
			         esp_err_to_name(dap_tcp_err));
		}
	}
	BIRDIEBOT_LOG_INTERNAL_HEAP("after dap_tcp");

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
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", BIRDIEBOT_TCP_TASK_STACK, NULL,
                            CONFIG_BIRDIEBOT_USBIP_TCP_PRIORITY, NULL, TCP_TASK_AFFINITY);
    /* 3072: with B1 (BIRDIEBOT_USBIP_PIPELINE) the DAP task also builds/sends
     * the USBIP RET_SUBMIT for a parked IN URB, so it needs lwIP send stack. */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    {
        /* Bring-up diagnostics: on the single-core C5 the internal heap is much
         * smaller than on the S3; a failed task create must be visible. */
        BaseType_t dap_task_ret =
            xTaskCreatePinnedToCore(DAP_Thread, "DAP_Task", 3072, NULL,
                                    CONFIG_BIRDIEBOT_DAP_TASK_PRIORITY, NULL,
                                    DAP_TASK_AFFINITY);
        ESP_LOGI("BIRDIEBOT", "DAP task create: %s (internal free=%u largest=%u)",
                 dap_task_ret == pdPASS ? "ok" : "FAILED",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT));
        if (dap_task_ret != pdPASS) {
            ESP_LOGE("BIRDIEBOT", "DAP task creation failed: no internal RAM");
            dap_handle_note_task_create(false);
        }
    }
#else
    if (xTaskCreatePinnedToCore(DAP_Thread, "DAP_Task", 4096, NULL,
                            CONFIG_BIRDIEBOT_DAP_TASK_PRIORITY, NULL,
                            DAP_TASK_AFFINITY) != pdPASS) {
        dap_handle_note_task_create(false);
    }
#endif

#if defined(CONFIG_SPIRAM) && (defined(CONFIG_IDF_TARGET_ESP32C5) || \
                                defined(CONFIG_IDF_TARGET_ESP32S3))
	/* UART bridge front task: background/low-frequency with a 4 KiB stack.
	 * On the single-core C5 and on the S3 N16R8 PSRAM build the stack goes to
	 * PSRAM (memory policy: only the stack, not the UART driver DMA buffers;
	 * the task never writes flash).  Fall back to internal RAM if the PSRAM
	 * allocation fails. */
	{
		BaseType_t uart_task_ret =
			xTaskCreateWithCaps(uart_bridge_task, "uart_server", 4096, NULL, 2,
			                    NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (uart_task_ret != pdPASS) {
			ESP_LOGW("BIRDIEBOT", "uart_server PSRAM stack unavailable, "
			         "using internal RAM");
			xTaskCreate(uart_bridge_task, "uart_server", 4096, NULL, 2, NULL);
		}
	}
#else
	xTaskCreate(uart_bridge_task, "uart_server", 4096, NULL, 2, NULL);
#endif

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !defined(CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID)
	/* New image after OTA: confirm within 10 s or the next reset rolls back. */
	ota_confirm_schedule();
#endif
}
