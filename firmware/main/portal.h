// Setup portal: the setup page, the /api/wifi endpoints and the captive-portal redirect.
#pragma once

#include "esp_http_server.h"

void portal_register(httpd_handle_t server);  // 5 URI handlers
esp_err_t portal_page(httpd_req_t *req);      // "/" while in setup mode
esp_err_t portal_redirect(httpd_req_t *req);  // unknown paths while in setup mode
