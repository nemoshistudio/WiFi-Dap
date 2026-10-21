#include "web_server.h"
#include "web_uri_module.h"
#include "ssdp.h"

#include <sdkconfig.h>

#include <esp_http_server.h>
#include <esp_event.h>

#include <esp_system.h>
#include <esp_log.h>
#include <sys/unistd.h>

#define TAG "web server"

#define WEB_WS_URI "/ws"

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
	/* docs/ota-local-upload.md §3.2: must be >= the 10 s OTA upload idle
	 * timeout so a slow/silent upload is detected as a timeout (HTTP 408) and
	 * not as a generic socket error. Also used as the per-request recv timeout
	 * by esp_http_server. */
	config.recv_wait_timeout = 10;
	/* URI handlers: 4x /api/target + /api/dap/kick (uri_target.c) + /ws, /api,
	 * /api/ota/upload and the "/" SPA fallback = 9 with the default limit, so
	 * keep headroom for future endpoints. */
	config.max_uri_handlers = 14;

	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if ((err = httpd_start(&http_server, &config)) != ESP_OK) {
		ESP_LOGE(TAG, "Error starting server!");
		ESP_ERROR_CHECK(err);
	}

	/* Target-flash endpoints must be registered BEFORE the URI modules: the
	 * "/" SPA fallback matches every GET path, so /api/target/info would be
	 * served as index.html otherwise (httpd checks handlers in registration
	 * order). */
#if CONFIG_BIRDIEBOT_TARGET_FLASH_ENABLE
	ESP_LOGI(TAG, "Registering target flash URI handlers");
	uri_target_register(http_server);
#endif

	ESP_LOGI(TAG, "Registering URI handlers");
	uri_module_init(http_server);
}

int server_is_running()
{
	return http_server != NULL;
}
