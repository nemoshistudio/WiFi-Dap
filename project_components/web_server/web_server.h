#ifndef WEB_SERVER_H_GUARD
#define WEB_SERVER_H_GUARD

#include <stdint.h>
#include <esp_event_base.h>

void disconnect_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data);
void connect_handler(void* arg, esp_event_base_t event_base,
                     int32_t event_id, void* event_data);

void start_webserver(void);
int server_is_running();

/**
 * @brief Clean up per-socket WS state after an abnormal disconnect.
 *
 * Implemented in uri_modules/uri_ws.c and called from the httpd close_fn
 * (TCP RST / timeout path, where no WebSocket CLOSE frame is received).
 * @note internal integration hook; not part of the public device API.
 */
void web_ws_socket_closed(int fd);


#endif //WEB_SERVER_H_GUARD