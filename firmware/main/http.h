#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_server.h"

void http_start(void);  // REST, WebSocket /ws, MCP /mcp and the dashboard on CONFIG_BRIDGE_HTTP_PORT

// Shared with portal.c.
bool http_origin_ok(httpd_req_t *req);
esp_err_t http_send_json(httpd_req_t *req, int status, cJSON *obj);  // takes obj
esp_err_t http_send_error(httpd_req_t *req, const char *code, const char *message);
