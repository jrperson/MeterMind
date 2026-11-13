// Meter core: one FreeRTOS task owns the USB serial link, runs queued jobs and polls the meter
// when idle. Port of bridge/meter.py; PROTOCOL.md explains every quirk handled here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define METER_SETTLE_MAX_S 3.5

typedef struct {
    char code[24];     // API error code: bad_request, verify_failed, meter_timeout, ...
    char message[160];
} merr_t;

typedef struct {
    int function;              // PROTO_FUNCS index, -1 unknown
    bool range_valid;          // range parsed from the label
    double range;
    bool range_label_valid;    // label known (it may be "")
    char range_label[24];
    int autorange;             // -1 unknown, 0, 1
    int rate;                  // PROTO_RATE index, -1 unknown
    int beep;                  // -1 unknown, 0, 1
    char temp_unit[8];
    bool settled;
} meter_state_t;

typedef struct {
    uint32_t seq;
    int64_t t_ms;              // ms since boot
    int64_t ts_ms;             // unix time in ms, 0 when the clock is not set
    bool overload;
    double value;
    int function;
    char unit[8];
    bool range_label_valid;
    char range_label[24];
    bool display_valid;
    char display[40];
} meter_sample_t;

typedef struct {
    char manufacturer[24], model[24], serial[24], meter_fw[16], idn[96];
} meter_info_t;

typedef enum { MEV_READING, MEV_STATE, MEV_STATUS, MEV_LEADS } mev_type_t;

typedef struct {
    mev_type_t type;
    union {
        meter_sample_t reading;
        meter_state_t state;
        struct {
            bool connected, port_open;
        } status;
        int leads;  // MEV_LEADS: bridge_lead_name() index (where the user says the red lead is)
    };
} meter_event_t;

enum { RANGE_NONE = 0, RANGE_AUTO = 1, RANGE_VALUE = 2 };

typedef struct {
    int function;     // -1 = leave alone
    int range_kind;   // RANGE_NONE, RANGE_AUTO, RANGE_VALUE
    double range;
    int rate;         // -1 = leave alone
    int beep;         // -1 = leave alone
} meter_cfg_t;

enum { CH_FUNCTION = 1, CH_RANGE = 2, CH_RATE = 4, CH_BEEP = 8 };

typedef struct {
    uint32_t changed;  // CH_* bits
    uint32_t seq;
    bool settling;
} meter_cfg_result_t;

void meter_start(void);

bool meter_connected(void);
bool meter_port_open(void);
void meter_get_state(meter_state_t *out);
bool meter_last_sample(meter_sample_t *out);
uint32_t meter_seq(void);
// Send an event to every subscriber as if the meter task had (any task may call it).
void meter_post_event(const meter_event_t *ev);
void meter_get_info(meter_info_t *out);
int64_t meter_millis(void);

// Event subscription: every reading/state/status event is copied to each subscriber queue
// (a full queue drops its oldest event rather than blocking the meter task).
QueueHandle_t meter_subscribe(void);
void meter_unsubscribe(QueueHandle_t q);

// Jobs run on the meter task; these block until the job is done. Arguments must already be
// validated (bridge.c). Return false with err filled on failure.
bool meter_configure(const meter_cfg_t *cfg, meter_cfg_result_t *res, merr_t *err);
bool meter_scpi(const char *cmd, char *reply, size_t reply_len, bool *has_reply, merr_t *err);

// Asks the meter task to switch the meter's VBUS off for a second (resets its CH340).
void meter_request_vbus_cycle(void);
