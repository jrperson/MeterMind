#include "usb_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp_ch34x.h"

static const char *TAG = "link";

#define BAUD 115200
#define RX_BUF 2048

static StreamBufferHandle_t s_rx;
static cdc_acm_dev_hdl_t s_dev;
static volatile bool s_gone;
static char s_acc[256];  // received bytes not yet returned as a line
static size_t s_acc_len;

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    xStreamBufferSend(s_rx, data, len, 0);  // never blocks; overflow drops bytes (resync handles it)
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *ev, void *arg)
{
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "meter USB disconnected");
        s_gone = true;
    } else if (ev->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "CDC error %d", ev->data.error);
    }
}

void link_init(void)
{
    s_rx = xStreamBufferCreate(RX_BUF, 1);
    usb_host_config_t host = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
}

bool link_open(uint32_t wait_ms)
{
    if (s_dev) return true;
    cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = wait_ms,
        .out_buffer_size = 256,
        .in_buffer_size = 256,
        .event_cb = on_event,
        .data_cb = on_rx,
    };
    s_gone = false;
    // The meter's CH340 is 1a86:7523 (PROTOCOL.md). CH34X_PID_AUTO would wait the full timeout on
    // each of the other CH34x PIDs first.
    if (ch34x_vcp_open(CH340_PID_1, 0, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    cdc_acm_line_coding_t lc = {.dwDTERate = BAUD, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8};
    if (cdc_acm_host_line_coding_set(s_dev, &lc) != ESP_OK) {
        ESP_LOGW(TAG, "could not set line coding");
        link_close();
        return false;
    }
    cdc_acm_host_set_control_line_state(s_dev, true, true);  // as pyserial does on open
    ESP_LOGI(TAG, "meter CH340 open at %d 8N1", BAUD);
    vTaskDelay(pdMS_TO_TICKS(200));
    link_reset_input();
    return true;
}

bool link_is_open(void)
{
    if (s_dev && s_gone) link_close();
    return s_dev != NULL;
}

void link_close(void)
{
    if (s_dev) cdc_acm_host_close(s_dev);
    s_dev = NULL;
    s_gone = false;
    link_reset_input();
}

bool link_write_line(const char *cmd)
{
    if (!link_is_open()) return false;
    uint8_t buf[256];
    size_t n = strlen(cmd);
    if (n + 1 > sizeof buf) return false;
    memcpy(buf, cmd, n);
    buf[n++] = '\n';
    if (cdc_acm_host_data_tx_blocking(s_dev, buf, n, 200) != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s", cmd);
        return false;
    }
    return true;
}

int link_read_line(char *buf, size_t len, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        char *nl = memchr(s_acc, '\n', s_acc_len);
        if (nl) {
            size_t n = (size_t)(nl - s_acc);
            size_t keep = s_acc_len - n - 1;
            while (n && (s_acc[n - 1] == '\r')) n--;
            size_t c = n < len - 1 ? n : len - 1;
            memcpy(buf, s_acc, c);
            buf[c] = 0;
            memmove(s_acc, nl + 1, keep);
            s_acc_len = keep;
            return (int)c;
        }
        if (s_acc_len == sizeof s_acc) s_acc_len = 0;  // runaway line without a newline: drop it
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0 || !link_is_open()) return -1;
        TickType_t t = pdMS_TO_TICKS((left_us + 999) / 1000);
        size_t got = xStreamBufferReceive(s_rx, s_acc + s_acc_len, sizeof s_acc - s_acc_len, t ? t : 1);
        s_acc_len += got;
    }
}

void link_reset_input(void)
{
    uint8_t junk[64];
    while (xStreamBufferReceive(s_rx, junk, sizeof junk, 0) > 0) {}
    s_acc_len = 0;
}

void link_drain(uint32_t quiet_ms)
{
    uint8_t junk[64];
    while (xStreamBufferReceive(s_rx, junk, sizeof junk, pdMS_TO_TICKS(quiet_ms)) > 0) {}
    s_acc_len = 0;
}
