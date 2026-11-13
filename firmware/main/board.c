#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "meter.h"
#include "net.h"

static const char *TAG = "board";

#define UI_TICK_MS 20
#define SETUP_HOLD_MS 3000
#define LED_LEVEL 24  // of 255: the SK6812 is very bright behind a thin board

static led_strip_handle_t s_led;

void board_init(void)
{
    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_METER_PWR_EN, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = (1ULL << PIN_METER_FLT_N) | (1ULL << PIN_BOOT), .mode = GPIO_MODE_INPUT};
    gpio_config(&in);  // both have external pull-ups (R8, R6)
    board_meter_vbus(true);

    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "status LED unavailable");
        s_led = NULL;
    }
}

void board_meter_vbus(bool on)
{
    gpio_set_level(PIN_METER_PWR_EN, on);
    ESP_LOGI(TAG, "meter VBUS %s", on ? "on" : "off");
}

bool board_meter_vbus_fault(void) { return gpio_get_level(PIN_METER_FLT_N) == 0; }

static void led(int r, int g, int b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r * LED_LEVEL / 255, g * LED_LEVEL / 255, b * LED_LEVEL / 255);
    led_strip_refresh(s_led);
}

// Status colours:
//   blue blinking     setup mode: join WiFi "MeterMind-XXXX" and open http://192.168.4.1/
//   yellow blinking   setup mode, trying the network chosen on the setup page
//   yellow            connecting to WiFi
//   orange            on WiFi, no meter on the USB-A port (or it is not answering)
//   green             running; a cyan blink for every new reading
//   red blinking      meter VBUS overcurrent (U3 fault)
//   white             BOOT held: at 3 s setup mode starts (or ends, when a network is saved)
static void ui_task(void *arg)
{
    QueueHandle_t q = meter_subscribe();
    int flash = 0, held_ms = 0, tick = 0;
    bool fault_logged = false, fired = false;
    for (;;) {
        meter_event_t ev;
        while (q && xQueueReceive(q, &ev, 0) == pdTRUE)
            if (ev.type == MEV_READING) flash = 3;

        if (gpio_get_level(PIN_BOOT) == 0) {  // one toggle per press, however long it is held
            held_ms += UI_TICK_MS;
            if (held_ms >= SETUP_HOLD_MS && !fired) {
                fired = true;
                ESP_LOGW(TAG, "BOOT held 3 s: %s setup mode", net_setup_active() ? "leaving" : "entering");
                net_setup_toggle();
            }
            if (!fired) {
                led(255, 255, 255);
                vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
                continue;
            }
        } else {
            held_ms = 0;
            fired = false;
        }

        bool fault = board_meter_vbus_fault();
        if (fault && !fault_logged) ESP_LOGE(TAG, "meter VBUS overcurrent (U3 fault)");
        fault_logged = fault;
        bool blink = (tick / 25) % 2;  // 0.5 s on, 0.5 s off
        net_state_t n = net_get_state();
        if (fault) led(blink ? 255 : 0, 0, 0);
        else if (net_setup_active() && n == NET_CONNECTING) led(blink ? 255 : 0, blink ? 160 : 0, 0);
        else if (net_setup_active() || n == NET_IDLE) led(0, 0, blink ? 255 : 0);
        else if (n == NET_CONNECTING) led(255, 160, 0);
        else if (!meter_connected()) led(255, 60, 0);
        else if (flash > 0) led(0, 255, 255);
        else led(0, 255, 0);
        if (flash > 0) flash--;
        tick++;
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
    }
}

void board_start_ui(void) { xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL); }
