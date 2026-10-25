#include "web_server.h"
#include "web_uri_module.h"

#include <sdkconfig.h>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_heap_caps.h>

#include <esp_system.h>
#include <esp_log.h>
#include <sys/unistd.h>

#define TAG "web server"

#define WEB_WS_URI "/ws"

/* Bring-up memory accounting: on the single-core ESP32-C5 the internal heap is
 * tight enough that the web server footprint decides whether the DAP task can
 * be created.  Keep the per-stage numbers in the boot log. */
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define WEB_SERVER_LOG_INTERNAL_HEAP(stage)                                   \
	ESP_LOGI(TAG, "internal heap %-14s free=%u largest=%u", stage,            \
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |          \
	                                           MALLOC_CAP_8BIT),              \
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | \
	                                                    MALLOC_CAP_8BIT))
#else
#define WEB_SERVER_LOG_INTERNAL_HEAP(stage) ((void)0)
#endif

static httpd_handle_t http_server = NULL;

/**
 * Custom URI matcher (docs/protocol.md §1), 3 types of ref:
 *   - 1. "/"        : SPA entry, matches every GET path not matched earlier
 *   - 2. "/entry"   : exact match
 *   - 3. "/dir/"    : prefix match
 * plus the WS endpoint, which must match "/ws" and "/ws?<query>" (a query
 * string is tolerated; never match /ws.<asset>).
 */
static bool uri_match(const char *reference_uri, const char *uri_to_match, size_t match_upto)
{
	size_t ref_length = strlen(reference_uri);

	/* WS endpoint: "/ws" or "/ws?<query>" (query string is allowed). */
	if (ref_length == sizeof(WEB_WS_URI) - 1 &&
	    memcmp(reference_uri, WEB_WS_URI, ref_length) == 0) {
		if (match_upto < ref_length || memcmp(uri_to_match, WEB_WS_URI, ref_length) != 0) {
			return false;
		}
		return match_upto == ref_length || uri_to_match[ref_length] == '?';
	}

	/* ref should not be longer than the target */
	if (ref_length > match_upto) {
		return false;
	}

	/* strict matching "/entry" for ref = "/entry" */
	if (ref_length == match_upto) {
		return memcmp(reference_uri, uri_to_match, ref_length) == 0;
	}

	/* exact ref with a query string: "/api?..." must still reach the "/api"
	 * handler (e.g. cache busters / diagnostics). */
	if (ref_length < match_upto &&
	    uri_to_match[ref_length] == '?' &&
	    memcmp(reference_uri, uri_to_match, ref_length) == 0) {
		return true;
	}

	/* if ref = /dir/ targets are /dir/ and /dir/... */
	if (ref_length > 1 && reference_uri[ref_length - 1] == '/') {
		return memcmp(reference_uri, uri_to_match, ref_length) == 0;
	}

	/* Fall back to the SPA entry: handler "/" matches every other GET path. */
	if (ref_length == 1 && reference_uri[0] == '/') {
		return true;
	}

	return false;
}

static int8_t opened_socket = 0;

static esp_err_t web_server_on_open(httpd_handle_t hd, int sockfd)
{
	opened_socket++;
	ESP_LOGI(TAG, "%d open, now: %d", sockfd, opened_socket);
	return ESP_OK;
}

static void web_server_on_close(httpd_handle_t hd, int sockfd)
{
	opened_socket--;
	ESP_LOGI(TAG, "%d closed, now: %d", sockfd, opened_socket);
	/* Abnormal disconnect (TCP RST/timeout): no WS CLOSE frame was processed,
	 * so drop the client from the WS broadcast list (security review P2:
	 * stale fd cleanup). */
	web_ws_socket_closed(sockfd);
	close(sockfd);
}

void start_webserver(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	int err;

	config.enable_so_linger = true;
	config.linger_timeout = 0;
	config.lru_purge_enable = true;
	config.uri_match_fn = uri_match;
	config.open_fn = web_server_on_open;
	config.close_fn = web_server_on_close;
	config.keep_alive_enable = 1;
	config.keep_alive_count = 1;
	/* docs/ota-local-upload.md §3.2 (full-feature build): must be >= the 10 s
	 * OTA upload idle timeout so a slow/silent upload is detected as a
	 * timeout (HTTP 408) and not as a generic socket error. Also used as the
	 * per-request recv timeout by esp_http_server for the target-flash
	 * streaming upload, which is kept in the slim build. */
	config.recv_wait_timeout = 10;
	/* URI handlers in the slim default build (docs/feature-flags.md):
	 *   target flash (uri_target.c, CONFIG_BIRDIEBOT_TARGET_FLASH_ENABLE=y):
	 *     /api/target/{info,flash,status,reset,swd-clock}x2 + /api/dap/kick = 7
	 *     (/api/discover is compiled out: CONFIG_BIRDIEBOT_WEB_DISCOVER_ENABLE=n)
	 *   WEB_URI_MODULE modules: /api, 3x curve, /ws, "/" SPA fallback = 6
	 *     (/api/ota/upload is compiled out:
	 *      CONFIG_BIRDIEBOT_WEB_OTA_UPLOAD_ENABLE=n)
	 *   => 13.  The full-feature build adds /api/discover + /api/ota/upload
	 *   = 15.  Keep headroom (20) so re-enabling a switched-off endpoint does
	 *   not silently drop the last registration (WS/SPA). */
	config.max_uri_handlers = 20;
#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_SPIRAM)
	/* C5 internal-RAM budget: the httpd task is a low-frequency background
	 * task (web serving / OTA body receive); its 4 KiB stack can live in
	 * PSRAM.  Flash writes happen in the OTA worker task (internal stack), not
	 * here.  Keep the IDF default (internal) on every other target. */
	config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif

	WEB_SERVER_LOG_INTERNAL_HEAP("pre-httpd_start");
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if ((err = httpd_start(&http_server, &config)) != ESP_OK) {
		ESP_LOGE(TAG, "Error starting server!");
		ESP_ERROR_CHECK(err);
	}
	WEB_SERVER_LOG_INTERNAL_HEAP("post-httpd_start");

	/* Target-flash endpoints must be registered BEFORE the URI modules: the
	 * "/" SPA fallback matches every GET path, so /api/target/info would be
	 * served as index.html otherwise (httpd checks handlers in registration
	 * order). */
#if CONFIG_BIRDIEBOT_TARGET_FLASH_ENABLE
	ESP_LOGI(TAG, "Registering target flash URI handlers");
	uri_target_register(http_server);
#endif
	WEB_SERVER_LOG_INTERNAL_HEAP("post-uri_target");

	ESP_LOGI(TAG, "Registering URI handlers");
	uri_module_init(http_server);
	WEB_SERVER_LOG_INTERNAL_HEAP("post-uri_modules");
}

int server_is_running()
{
	return http_server != NULL;
}
