// MeterMind firmware (ESP32-S3-MINI-1 on the MeterMind board). Puts a Digcreat XDM1041
// multimeter on the LAN: REST, WebSocket, MCP and raw SCPI, as specified in ../API.md. The
// structure mirrors the Python prototype in ../bridge/ (meter.c = meter.py, bridge.c = api.py,
// mcp.c = mcp.py, http.c + scpi_server.c = server.py).
#include "board.h"
#include "bridge.h"
#include "console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http.h"
#include "mcp.h"
#include "meter.h"
#include "net.h"
#include "nvs_flash.h"
#include "scpi_server.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "MeterMind %s", bridge_version());

    board_init();      // meter VBUS on first, so the CH340 is enumerating while WiFi starts
    bridge_init();
    mcp_init();
    meter_start();
    net_start();
    http_start();
    scpi_server_start();
    board_start_ui();
    console_start();
}
