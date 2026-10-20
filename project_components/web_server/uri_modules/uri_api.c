#include "web_uri_module.h"
#include "api_json_router.h"
#include "web_host_guard.h"
#include "web_error_json.h"
#include "request_runner.h"
#include "memory_pool.h"

#include <esp_http_server.h>
#include <esp_log.h>

#include <cJSON.h>

#define TAG __FILE_NAME__

typedef struct post_request_t {
	api_json_req_t json;
	api_json_module_async_t async;
	httpd_req_t *req_out;
	char buf[0];
} post_request_t;

static void async_send_out_cb(void *arg, int module_status);
static int uri_api_send_out(httpd_req_t *req, post_request_t *post_req, int err);

/** Extract module/cmd from a parsed JSON request (-1 when absent/invalid). */
static void api_json_get_context(cJSON *json, int *module, int *cmd)
{
	*module = -1;
	*cmd = -1;
	if (json == NULL) {
		return;
	}
	cJSON *module_json = cJSON_GetObjectItem(json, "module");
	cJSON *cmd_json = cJSON_GetObjectItem(json, "cmd");
	if (cJSON_IsNumber(module_json)) {
		*module = module_json->valueint;
	}
	if (cJSON_IsNumber(cmd_json)) {
		*cmd = cmd_json->valueint;
	}
}

/** HTTP status + JSON code/text for an api_json_req_status_e (docs/protocol.md §1.1). */
static void api_err_status(api_json_req_status_e ret, const char **status,
                           const char **text, int *code)
{
	switch (ret) {
	case API_JSON_BAD_REQUEST:
		*status = "400 Bad Request";
		*text = "Bad json request";
		*code = 2;
		break;
	case API_JSON_UNSUPPORTED_CMD:
		*status = "400 Bad Request";
		*text = "Unsupported cmd";
		*code = 4;
		break;
	case API_JSON_PROPERTY_ERR:
		*status = "400 Bad Request";
		*text = "Property error";
		*code = 5;
		break;
	case API_JSON_BUSY:
		*status = "503 Service Unavailable";
		*text = "Resource busy";
		*code = 6;
		break;
	case API_JSON_INTERNAL_ERR:
	case API_JSON_OK:
	case API_JSON_ASYNC:
	default:
		*status = "500 Internal Server Error";
		*text = "Internal error";
		*code = 3;
		break;
	}
}

/** Send a uniform JSON error body (module/cmd kept when known, F10). */
static esp_err_t api_send_error(httpd_req_t *req, const char *status, int module,
                                int cmd, const char *text, int code)
{
	char buf[192];
	int n = web_error_json_format(buf, sizeof(buf), module, cmd, text, code);

	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, n > 0 ? buf : "", n > 0 ? (size_t) n : 0);
}

/** Send the error body matching an api_json_req_status_e. */
static esp_err_t api_send_route_error(httpd_req_t *req, cJSON *json, api_json_req_status_e ret)
{
	int module;
	int cmd;
	const char *status;
	const char *text;
	int code;

	api_json_get_context(json, &module, &cmd);
	api_err_status(ret, &status, &text, &code);
	return api_send_error(req, status, module, cmd, text, code);
}


static esp_err_t api_post_handler(httpd_req_t *req)
{
	uint32_t buf_len;
	int data_len;
	int err = ESP_OK;
	post_request_t *post_req;
	char *buf;
	uint32_t remaining = req->content_len;

	/* Browser Host/Origin guard (CSWSH / DNS rebinding; always active, this is
	 * not a login check). Reject before reading the body so no module/command
	 * runs for a cross-origin or DNS-rebinding request. */
	/* TODO(security review P2): per-fd request rate limiting for /api
	 * (e.g. 20 req/5 s) is not implemented yet; the shared 7 x 2 KiB
	 * memory_pool already bounds the damage and the request queue returns
	 * code=6 when exhausted. Tracked as a Wave3 hardening leftover. */
	if (!web_host_guard_http_ok(req)) {
		return api_send_error(req, "403 Forbidden", -1, -1, "forbidden origin", 5);
	}

	buf_len = memory_pool_get_buf_size() - sizeof(post_request_t);
	if (unlikely(buf_len < remaining)) {
		ESP_LOGE(TAG, "req size %lu > buf_len %lu", remaining, buf_len);
		return api_send_error(req, "413 Payload Too Large", -1, -1,
		                      "Bad json request", 2);
	}

	post_req = memory_pool_get(pdMS_TO_TICKS(20));
	if (unlikely(post_req == NULL)) {
		ESP_LOGE(TAG, "static buf busy");
		return api_send_error(req, "503 Service Unavailable", -1, -1,
		                      "Resource busy", 6);
	}
	buf = post_req->buf;

	data_len = httpd_req_recv(req, buf, buf_len);
	if (unlikely(data_len <= 0)) {
		ESP_LOGE(TAG, "httpd recv error");
		err = api_send_error(req, "400 Bad Request", -1, -1, "Bad json request", 2);
		goto put_buf;
	}

	/* Do not log the raw JSON body at INFO level: it may contain WiFi
	 * passwords or other secrets. */
	ESP_LOGI(TAG, "api post: fd %d, %d bytes", httpd_req_to_sockfd(req), data_len);
	ESP_LOGI(TAG, "heap min: %lu, cur: %lu", esp_get_minimum_free_heap_size(), esp_get_free_heap_size());

	/* Decode */
	post_req->json.in = cJSON_ParseWithLength(buf, data_len);
	if (unlikely(post_req->json.in == NULL)) {
		(void)api_send_error(req, "400 Bad Request", -1, -1, "Bad json request", 2);
		goto put_buf;
	}

	post_req->json.out = NULL;
	err = api_json_route(&post_req->json, &post_req->async);
	if (err == API_JSON_ASYNC) {
		httpd_req_async_handler_begin(req, &post_req->req_out);
		post_req->async.req_task.send_out.cb = async_send_out_cb;
		post_req->async.req_task.send_out.arg = post_req;
		if (req_queue_push_long_run(&post_req->async.req_task, pdMS_TO_TICKS(20))) {
			/* queued failed */
			httpd_req_async_handler_complete(post_req->req_out);
			(void)api_send_route_error(req, post_req->json.in, API_JSON_BUSY);
			goto put_buf;
		}
		return ESP_OK;
	} else if (unlikely(err != API_JSON_OK)) {
		(void)api_send_route_error(req, post_req->json.in, (api_json_req_status_e) err);
		goto put_buf;
	}

	if (post_req->json.out == NULL) {
		goto end;
	}

	/* api function returns something, send back to http client */
	err = uri_api_send_out(req, post_req, 0);
	goto put_buf;

end:
	cJSON_Delete(post_req->json.in);
	err = httpd_resp_send(req, NULL, 0);
	if (unlikely(err)) {
		ESP_LOGE(TAG, "resp_send err: %s", esp_err_to_name(err));
	}
put_buf:
	memory_pool_put(post_req);
	return err;
}

int uri_api_send_out(httpd_req_t *req, post_request_t *post_req, int err)
{
	char *buf;
	uint32_t buf_len;

	buf = post_req->buf;
	buf_len = memory_pool_get_buf_size() - sizeof(post_request_t);
	cJSON_Delete(post_req->json.in);
	if (post_req->json.out) {
		ESP_LOGI(TAG, "json out ok");
		httpd_resp_set_type(req, HTTPD_TYPE_JSON);
		err = !cJSON_PrintPreallocated(post_req->json.out, buf, buf_len - 5, 0);
		cJSON_Delete(post_req->json.out);
	}

	if (unlikely(err)) {
		httpd_resp_set_status(req, HTTPD_500);
		return httpd_resp_send(req, NULL, 0);
	}

	return httpd_resp_send(req, buf, strlen(buf));
}

void async_send_out_cb(void *arg, int module_status)
{
	post_request_t *req = arg;
	int module;
	int cmd;

	if (module_status != API_JSON_OK) {
		/* Uniform error body with the original request context (F10). */
		api_json_get_context(req->json.in, &module, &cmd);
		(void)api_send_route_error(req->req_out, req->json.in,
		                           (api_json_req_status_e) module_status);
		cJSON_Delete(req->json.in);
		httpd_req_async_handler_complete(req->req_out);
		memory_pool_put(req);
		return;
	}

	uri_api_send_out(req->req_out, req, module_status);

	/* clean resources */
	httpd_req_async_handler_complete(req->req_out);
	memory_pool_put(req);
};

/**
 * REGISTER MODULE
 * */

static const httpd_uri_t uri_api = {
	.uri       = "/api",
	.method    = HTTP_POST,
	.handler   = api_post_handler,
	.user_ctx  = NULL
};

static int URI_API_INIT(const httpd_uri_t **uri_conf) {
	*uri_conf = &uri_api;
	return 0;
}

static int URI_API_EXIT(const httpd_uri_t **uri_conf) {
	*uri_conf = &uri_api;
	return 0;
}

WEB_URI_MODULE_REGISTER(0x81, URI_API_INIT, URI_API_EXIT)


