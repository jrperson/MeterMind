// Line-based SCPI over TCP (PyVISA: TCPIP::<host>::5025::SOCKET). Mirrors the meter: queries
// get one reply line, unsupported queries get no reply. Every command goes through the bridge's
// queue and pacing, like POST /api/scpi.
#include "scpi_server.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "scpi";
#define MAX_CLIENTS 2

static volatile int s_clients;

static void handle_line(int fd, char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;
    cJSON *cmd = cJSON_CreateString(line);
    berr_t e = {0};
    cJSON *res = bridge_scpi(cmd, &e);
    cJSON_Delete(cmd);
    if (!res) {
        ESP_LOGW(TAG, "%s: %s", line, e.message);
        bridge_err_free(&e);
        return;
    }
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(res, "reply");
    if (cJSON_IsString(reply)) {
        send(fd, reply->valuestring, strlen(reply->valuestring), 0);
        send(fd, "\n", 1, 0);
    }
    cJSON_Delete(res);
}

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char line[256], buf[128];
    size_t len = 0;
    for (;;) {
        int n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[len] = 0;
                handle_line(fd, line);
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = buf[i];
            }
        }
    }
    close(fd);
    s_clients--;
    vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
    // Dual stack like esp_http_server: an IPv6 socket on in6addr_any also accepts IPv4, so clients
    // that resolve metermind.local to its IPv6 link-local address first still get in.
    int ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(CONFIG_BRIDGE_SCPI_PORT), .sin6_addr = IN6ADDR_ANY_INIT};
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 2)) {
        ESP_LOGE(TAG, "cannot listen on port %d", CONFIG_BRIDGE_SCPI_PORT);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "raw SCPI on tcp port %d", CONFIG_BRIDGE_SCPI_PORT);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        if (s_clients >= MAX_CLIENTS) {
            close(fd);
            continue;
        }
        s_clients++;
        if (xTaskCreate(client_task, "scpi_client", 4096, (void *)(intptr_t)fd, 4, NULL) != pdPASS) {
            close(fd);
            s_clients--;
        }
    }
}

void scpi_server_start(void)
{
    if (CONFIG_BRIDGE_SCPI_PORT) xTaskCreate(listen_task, "scpi", 3072, NULL, 4, NULL);
}
