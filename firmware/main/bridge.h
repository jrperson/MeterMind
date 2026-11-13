// Operation layer shared by every transport (HTTP, WebSocket, MCP, raw SCPI). Port of
// bridge/api.py: transports only translate their wire format to these calls, so behaviour is
// identical whichever way a client connects. API.md is the contract.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "meter.h"

typedef struct {
    char code[24];
    char message[320];
    cJSON *extra;  // optional fields merged into the error body (owned)
} berr_t;

void bridge_init(void);
const char *bridge_version(void);

int bridge_http_status(const char *code);
cJSON *bridge_error_body(berr_t *e);  // {"error", "message", ...extra}; takes e->extra
void bridge_err_free(berr_t *e);

// Each returns a new cJSON object, or NULL with *e filled.
cJSON *bridge_info(void);
cJSON *bridge_state(berr_t *e);
cJSON *bridge_reading(bool fresh, int timeout_ms, berr_t *e);
cJSON *bridge_config(const cJSON *body, berr_t *e);
cJSON *bridge_measure(const cJSON *body, berr_t *e);
cJSON *bridge_scpi(const cJSON *cmd, berr_t *e);  // {"reply": string or null}
cJSON *bridge_leads(void);                         // {"red": unknown|none|input|ma|10a}
cJSON *bridge_set_leads(const cJSON *body, berr_t *e);
const char *bridge_lead_name(int i);

// JSON for meter events as pushed on the WebSocket ({"type": "reading", ...}).
cJSON *bridge_event_json(const meter_event_t *ev);
cJSON *bridge_sample_json(const meter_sample_t *s, bool with_age);
