#include "captive_dns.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "dns";
#define TTL_S 10  // short, so nothing keeps the fake answers once setup is over

static volatile bool s_run;
static TaskHandle_t s_task;  // set by xTaskCreate before the task runs
static uint32_t s_ip;

// Turns the query in p (n bytes) into its reply in place. Returns the reply length, or -1 to
// stay silent. Only the question is kept (EDNS and other extra records are dropped).
static int build_reply(uint8_t *p, int n, int cap)
{
    if (n < 12) return -1;
    if (p[2] & 0x80) return -1;               // a response, not a query
    if ((p[2] >> 3) & 0x0F) return -1;        // not a standard query
    if (p[4] != 0 || p[5] != 1) return -1;    // exactly one question
    int i = 12;
    while (i < n && p[i]) {
        if (p[i] & 0xC0) return -1;           // no compression in a question
        i += p[i] + 1;
    }
    if (i + 5 > n) return -1;                 // root label, type and class
    int qtype = p[i + 1] << 8 | p[i + 2];
    int qclass = (p[i + 3] << 8 | p[i + 4]) & 0x7FFF;
    int len = i + 5;
    p[2] = 0x84 | (p[2] & 0x01);              // response, authoritative, keep RD
    p[3] = 0x00;                              // no error
    memset(p + 6, 0, 6);                      // answer, authority and additional counts
    if (qtype == 1 && qclass == 1) {          // A IN: one answer; anything else gets none
        if (len + 16 > cap) return -1;
        uint8_t *a = p + len;
        const uint8_t rr[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, TTL_S, 0, 4};
        memcpy(a, rr, sizeof rr);
        memcpy(a + 12, &s_ip, 4);
        len += 16;
        p[7] = 1;
    }
    return len;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "cannot listen on UDP 53");
        if (sock >= 0) close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);  // to notice s_run going false
    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        int len = build_reply(buf, n, sizeof buf);
        if (len > 0) sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(uint32_t ip)
{
    s_ip = ip;
    if (s_run && s_task) return;
    for (int i = 0; s_task && i < 40; i++) vTaskDelay(pdMS_TO_TICKS(50));  // a stopping task still holds the port
    s_run = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        s_run = false;
    }
}

void captive_dns_stop(void) { s_run = false; }
