// Serial console on UART0, reached through the USB-C port (CH340C) at 115200 baud, for example
// with `idf.py monitor`. A second way to set WiFi, besides the setup network.
#include "console.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bridge.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter.h"
#include "net.h"
#include "sdkconfig.h"

#define MAX_ARGS 4

static void help(void)
{
    printf("commands:\n"
           "  status                      WiFi, meter and memory\n"
           "  wifi <ssid> [password]      save WiFi settings and connect (quote names with spaces)\n"
           "  wifi clear                  erase WiFi settings and restart (into setup mode)\n"
           "  setup [off]                 open (or close) the setup network\n"
           "  vbus cycle                  switch the meter's USB power off for 1 s\n"
           "  reboot\n"
           "Holding BOOT for 3 s also opens (or closes) the setup network.\n");
}

static void status(void)
{
    char line[240];
    net_describe(line, sizeof line);
    printf("%s\n", line);
    meter_info_t i;
    meter_get_info(&i);
    if (meter_connected()) printf("meter: %s %s serial %s, fw %s\n", i.manufacturer, i.model, i.serial, i.meter_fw);
    else printf("meter: %s\n", meter_port_open() ? "USB open, not answering" : "not connected");
    printf("meter VBUS: %s\n", board_meter_vbus_fault() ? "OVERCURRENT" : "ok");
    meter_sample_t s;
    if (meter_last_sample(&s)) printf("last reading: %s (seq %lu)\n", s.display_valid ? s.display : "?", (unsigned long)s.seq);
    printf("bridge %s, up %lld s, free heap %u (min %u)\n", bridge_version(), esp_timer_get_time() / 1000000,
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

// Splits a line into words; "double quotes" group words with spaces.
static int split(char *s, char **argv)
{
    int n = 0;
    while (*s && n < MAX_ARGS) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    return n;
}

static void run(char *line)
{
    char *argv[MAX_ARGS];
    int argc = split(line, argv);
    if (!argc) return;
    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) help();
    else if (!strcmp(argv[0], "status")) status();
    else if (!strcmp(argv[0], "reboot")) esp_restart();
    else if (!strcmp(argv[0], "vbus") && argc == 2 && !strcmp(argv[1], "cycle")) {
        meter_request_vbus_cycle();
        printf("meter VBUS will be cycled\n");
    } else if (!strcmp(argv[0], "setup") && argc == 1) {
        net_setup_start();
        printf("setup network starting\n");
    } else if (!strcmp(argv[0], "setup") && argc == 2 && !strcmp(argv[1], "off")) {
        net_setup_stop();
        printf("setup network closing\n");
    } else if (!strcmp(argv[0], "wifi") && argc == 2 && !strcmp(argv[1], "clear")) {
        net_clear_credentials();
        printf("WiFi settings erased; restarting\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(argv[0], "wifi") && (argc == 2 || argc == 3)) {
        if (net_set_credentials(argv[1], argc == 3 ? argv[2] : "")) printf("saved; connecting to \"%s\"\n", argv[1]);
        else printf("could not save (SSID up to 32 bytes, password up to 63)\n");
    } else {
        printf("unknown command; type help\n");
    }
}

static void console_task(void *arg)
{
    char line[160];
    size_t len = 0;
    printf("\nMeterMind console: type help\n> ");
    fflush(stdout);
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[len] = 0;
            run(line);
            len = 0;
            printf("> ");
        } else if ((c == 8 || c == 127) && len) {
            len--;
            fputs("\b \b", stdout);
        } else if (c >= 32 && c < 127 && len < sizeof line - 1) {
            line[len++] = (char)c;
            putchar(c);
        }
        fflush(stdout);
    }
}

void console_start(void)
{
    const int port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK) return;
    uart_vfs_dev_use_driver(port);  // blocking reads on stdin
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
}
