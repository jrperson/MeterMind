#include "bridge.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "proto.h"

#define MAX_SAMPLES 100

static SemaphoreHandle_t op_lock;  // config/measure/scpi run one at a time

static const char *const CONFIG_KEYS[] = {"beep", "force", "function", "range", "rate"};
static const char *const MEASURE_KEYS[] = {"beep", "force", "function", "range", "rate", "samples", "timeout_ms"};

// Where the user says the red lead is (the black one is always in COM). The bridge cannot see the
// leads; this is what the user told it, "unknown" until they do (and again after a restart).
enum { LEAD_UNKNOWN, LEAD_NONE, LEAD_INPUT, LEAD_MA, LEAD_10A, NLEADS };
static const char *const LEAD_NAMES[NLEADS] = {"unknown", "none", "input", "ma", "10a"};
static const char *const JACK_NAMES[NLEADS] = {"", "", "Input", "µA mA", "10A"};
static const char *const FUNC_TITLES[PROTO_NFUNCS] = {"DC volts", "AC volts", "DC current", "AC current",
    "Resistance", "Capacitance", "Frequency", "Period", "Diode test", "Continuity", "Temperature"};
static volatile int s_red_lead = LEAD_UNKNOWN;

const char *bridge_lead_name(int i) { return i >= 0 && i < NLEADS ? LEAD_NAMES[i] : "unknown"; }
#define NKEYS(a) ((int)(sizeof(a) / sizeof(a[0])))

void bridge_init(void) { op_lock = xSemaphoreCreateMutex(); }

const char *bridge_version(void) { return esp_app_get_description()->version; }

// ------------------------------------------------------------------ errors
static void err(berr_t *e, const char *code, const char *fmt, ...)
{
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof e->message, fmt, ap);
    va_end(ap);
}

static void err_from_meter(berr_t *e, const merr_t *m)
{
    err(e, m->code[0] ? m->code : "internal", "%s", m->message);
}

static cJSON *extra(berr_t *e)
{
    if (!e->extra) e->extra = cJSON_CreateObject();
    return e->extra;
}

void bridge_err_free(berr_t *e)
{
    cJSON_Delete(e->extra);
    e->extra = NULL;
}

int bridge_http_status(const char *code)
{
    static const struct { const char *code; int status; } map[] = {
        {"bad_request", 400}, {"forbidden", 403}, {"busy", 409}, {"setup_required", 409}, {"leads_mismatch", 409}, {"internal", 500},
        {"verify_failed", 502}, {"meter_disconnected", 503}, {"meter_timeout", 504},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].code, code)) return map[i].status;
    return 500;
}

cJSON *bridge_error_body(berr_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", e->code);
    cJSON_AddStringToObject(o, "message", e->message);
    if (e->extra) {
        cJSON *it = e->extra->child;
        while (it) {
            cJSON *next = it->next;
            cJSON_AddItemToObject(o, it->string, cJSON_DetachItemViaPointer(e->extra, it));
            it = next;
        }
        bridge_err_free(e);
    }
    return o;
}

// ------------------------------------------------------------------ JSON helpers
static void add_str(cJSON *o, const char *key, const char *s, bool valid)
{
    if (valid) cJSON_AddStringToObject(o, key, s);
    else cJSON_AddNullToObject(o, key);
}

static void add_tri(cJSON *o, const char *key, int v)
{
    if (v < 0) cJSON_AddNullToObject(o, key);
    else cJSON_AddBoolToObject(o, key, v);
}

static cJSON *state_json(const meter_state_t *s)
{
    cJSON *o = cJSON_CreateObject();
    add_str(o, "function", s->function >= 0 ? PROTO_FUNCS[s->function].name : "", s->function >= 0);
    if (s->range_valid) cJSON_AddNumberToObject(o, "range", s->range);
    else cJSON_AddNullToObject(o, "range");
    add_str(o, "range_label", s->range_label, s->range_label_valid);
    add_tri(o, "autorange", s->autorange);
    add_str(o, "rate", s->rate >= 0 ? PROTO_RATE_NAMES[s->rate] : "", s->rate >= 0);
    add_tri(o, "beep", s->beep);
    cJSON_AddBoolToObject(o, "settled", s->settled);
    cJSON_AddStringToObject(o, "leads", LEAD_NAMES[s_red_lead]);
    return o;
}

cJSON *bridge_sample_json(const meter_sample_t *s, bool with_age)
{
    cJSON *o = cJSON_CreateObject();
    if (s->overload) cJSON_AddNullToObject(o, "value");
    else cJSON_AddNumberToObject(o, "value", s->value);
    add_str(o, "unit", s->unit, s->unit[0] != 0);
    add_str(o, "function", s->function >= 0 ? PROTO_FUNCS[s->function].name : "", s->function >= 0);
    add_str(o, "range_label", s->range_label, s->range_label_valid);
    cJSON_AddBoolToObject(o, "overload", s->overload);
    add_str(o, "display", s->display, s->display_valid);
    cJSON_AddNumberToObject(o, "seq", s->seq);
    cJSON_AddNumberToObject(o, "t_ms", (double)s->t_ms);
    if (s->ts_ms) cJSON_AddNumberToObject(o, "ts", s->ts_ms / 1000.0);
    if (with_age) {
        int64_t age = meter_millis() - s->t_ms;
        cJSON_AddNumberToObject(o, "age_ms", (double)(age > 0 ? age : 0));
    }
    return o;
}

static void prepend_type(cJSON *o, const char *type)
{
    cJSON *t = cJSON_CreateString(type);
    t->string = strdup("type");
    cJSON_InsertItemInArray(o, 0, t);  // works on objects too: it only relinks the child list
}

cJSON *bridge_event_json(const meter_event_t *ev)
{
    cJSON *o;
    switch (ev->type) {
    case MEV_READING:
        o = bridge_sample_json(&ev->reading, false);
        prepend_type(o, "reading");
        return o;
    case MEV_STATE:
        o = state_json(&ev->state);
        prepend_type(o, "state");
        return o;
    case MEV_LEADS:
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "leads");
        cJSON_AddStringToObject(o, "red", bridge_lead_name(ev->leads));
        return o;
    default:
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "status");
        cJSON_AddBoolToObject(o, "connected", ev->status.connected);
        cJSON_AddBoolToObject(o, "port_open", ev->status.port_open);
        return o;
    }
}

// ------------------------------------------------------------------ validation
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char **)a, *(const char **)b); }

static bool check_unknown(const cJSON *body, const char *const *allowed, int nallowed, berr_t *e)
{
    const char *unknown[16];
    int n = 0;
    for (const cJSON *it = body->child; it && n < 16; it = it->next) {
        bool ok = false;
        for (int i = 0; i < nallowed; i++) ok |= !strcmp(it->string, allowed[i]);
        if (!ok) unknown[n++] = it->string;
    }
    if (!n) return true;
    qsort(unknown, n, sizeof unknown[0], cmp_str);
    char list[160] = "";
    for (int i = 0; i < n; i++) {
        if (i) strlcat(list, ", ", sizeof list);
        strlcat(list, unknown[i], sizeof list);
    }
    err(e, "bad_request", "unknown field(s): %s", list);
    cJSON *a = cJSON_AddArrayToObject(extra(e), "allowed");
    for (int i = 0; i < nallowed; i++) cJSON_AddItemToArray(a, cJSON_CreateString(allowed[i]));
    return false;
}

static const cJSON *field(const cJSON *body, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(body, key);
    return cJSON_IsNull(it) ? NULL : it;  // null counts as absent, like body.get() in Python
}

static void join_names(char *out, size_t len, bool funcs)
{
    out[0] = 0;
    int n = funcs ? PROTO_NFUNCS : PROTO_NRATES;
    for (int i = 0; i < n; i++) {
        if (i) strlcat(out, ", ", len);
        strlcat(out, funcs ? PROTO_FUNCS[i].name : PROTO_RATE_NAMES[i], len);
    }
}

// Validates a config request. A function without a range means autorange: each meter function
// silently remembers its last manual range, which is a trap for automated clients.
static bool normalize_config(const cJSON *body, int current_function, const char *const *allowed,
                             int nallowed, meter_cfg_t *c, berr_t *e)
{
    if (!cJSON_IsObject(body)) {
        err(e, "bad_request", "body must be a JSON object");
        return false;
    }
    if (!check_unknown(body, allowed, nallowed, e)) return false;
    *c = (meter_cfg_t){.function = -1, .range_kind = RANGE_NONE, .rate = -1, .beep = -1};
    char names[128];

    const cJSON *func = field(body, "function");
    if (func) {
        int f = cJSON_IsString(func) ? proto_func_index(func->valuestring) : -1;
        if (f < 0) {
            join_names(names, sizeof names, true);
            err(e, "bad_request", "function must be one of %s", names);
            return false;
        }
        c->function = f;
    }
    int target = c->function >= 0 ? c->function : current_function;

    const cJSON *rng = field(body, "range");
    bool defaulted = !rng && c->function >= 0 && PROTO_FUNCS[c->function].nranges;
    if (rng || defaulted) {
        if (target >= 0 && !PROTO_FUNCS[target].nranges) {
            if (rng) {
                err(e, "bad_request", "function %s has no selectable range", PROTO_FUNCS[target].name);
                return false;
            }
        } else if (defaulted || (cJSON_IsString(rng) && !strcmp(rng->valuestring, "auto"))) {
            c->range_kind = RANGE_AUTO;
        } else if (cJSON_IsNumber(rng) && isfinite(rng->valuedouble) && rng->valuedouble > 0) {
            if (target >= 0 && !proto_select_range(target, rng->valuedouble, NULL)) {
                const proto_func_t *f = &PROTO_FUNCS[target];
                err(e, "bad_request", "%g exceeds the largest %s range (%g)", rng->valuedouble, f->name,
                    f->ranges[f->nranges - 1]);
                cJSON *a = cJSON_AddArrayToObject(extra(e), "ranges");
                for (int i = 0; i < f->nranges; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(f->ranges[i]));
                return false;
            }
            c->range_kind = RANGE_VALUE;
            c->range = rng->valuedouble;
        } else {
            err(e, "bad_request", "range must be \"auto\" or a positive number in SI units");
            return false;
        }
    }

    const cJSON *rate = field(body, "rate");
    if (rate) {
        c->rate = cJSON_IsString(rate) ? proto_rate_index(rate->valuestring) : -1;
        if (c->rate < 0) {
            join_names(names, sizeof names, false);
            err(e, "bad_request", "rate must be one of %s", names);
            return false;
        }
    }

    const cJSON *beep = field(body, "beep");
    if (beep) {
        if (!cJSON_IsBool(beep)) {
            err(e, "bad_request", "beep must be true or false");
            return false;
        }
        c->beep = cJSON_IsTrue(beep);
    }
    return true;
}

// ------------------------------------------------------------------ test leads
static void amps(char *out, size_t len, double fs)
{
    if (fs < 1e-3) snprintf(out, len, "%g µA", fs * 1e6);
    else if (fs < 1) snprintf(out, len, "%g mA", fs * 1e3);
    else snprintf(out, len, "%g A", fs);
}

// Why function f (on manual range fs, or autorange when fs < 0) does not suit the red lead's jack.
static bool leads_problem(int red, int f, double fs, char *msg, size_t len)
{
    if (red == LEAD_NONE || f < 0) return false;
    bool current = !strcmp(PROTO_FUNCS[f].name, "dci") || !strcmp(PROTO_FUNCS[f].name, "aci");
    const char *fix = " Move the lead and update the lead setting (dashboard or PUT /api/leads) first.";
    char r[24];
    if (red == LEAD_UNKNOWN && current) {
        snprintf(msg, len, "%s needs the red lead in the µA mA or 10A jack, and no lead position is set."
                 " Set where the leads are (dashboard or PUT /api/leads) first.", FUNC_TITLES[f]);
    } else if (red == LEAD_INPUT && current) {
        snprintf(msg, len, "%s needs the red lead in the µA mA or 10A jack, but it is set to the Input jack.%s",
                 FUNC_TITLES[f], fix);
    } else if ((red == LEAD_MA || red == LEAD_10A) && !current) {
        snprintf(msg, len, "%s needs the red lead in the Input jack, but it is set to the %s jack: measuring there"
                 " puts the input across the current shunt and can blow the fuse.%s", FUNC_TITLES[f], JACK_NAMES[red], fix);
    } else if (red == LEAD_MA && current && fs >= 5) {
        amps(r, sizeof r, fs);
        snprintf(msg, len, "The %s range needs the red lead in the 10A jack, but it is set to the µA mA jack.%s", r, fix);
    } else if (red == LEAD_10A && current && fs > 0 && fs <= 0.5) {
        amps(r, sizeof r, fs);
        snprintf(msg, len, "The %s range needs the red lead in the µA mA jack, but it is set to the 10A jack.%s", r, fix);
    } else {
        return false;
    }
    return true;
}

// The optional "force" flag skips the lead check (the dashboard sends it after the user confirms).
static bool take_force(const cJSON *body, bool *force, berr_t *e)
{
    const cJSON *f = cJSON_IsObject(body) ? field(body, "force") : NULL;
    *force = false;
    if (!f) return true;
    if (!cJSON_IsBool(f)) {
        err(e, "bad_request", "force must be true or false");
        return false;
    }
    *force = cJSON_IsTrue(f);
    return true;
}

// Refuses a function or range change that does not suit where the user says the lead is.
static bool check_leads(const meter_cfg_t *c, const meter_state_t *cur, berr_t *e)
{
    if (c->function < 0 && c->range_kind == RANGE_NONE) return true;
    int f = c->function >= 0 ? c->function : cur->function;
    double fs = -1;
    if (c->range_kind == RANGE_VALUE && f >= 0) proto_select_range(f, c->range, &fs);
    else if (c->range_kind == RANGE_NONE && cur->autorange == 0 && cur->range_valid) fs = cur->range;
    int red = s_red_lead;
    char msg[sizeof e->message];
    if (!leads_problem(red, f, fs, msg, sizeof msg)) return true;
    err(e, "leads_mismatch", "%s", msg);
    cJSON_AddStringToObject(extra(e), "leads", LEAD_NAMES[red]);
    cJSON_AddStringToObject(extra(e), "function", PROTO_FUNCS[f].name);
    return false;
}

cJSON *bridge_leads(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "red", LEAD_NAMES[s_red_lead]);
    return o;
}

cJSON *bridge_set_leads(const cJSON *body, berr_t *e)
{
    static const char *const KEYS[] = {"red"};
    if (!cJSON_IsObject(body)) {
        err(e, "bad_request", "body must be a JSON object");
        return NULL;
    }
    if (!check_unknown(body, KEYS, 1, e)) return NULL;
    const cJSON *red = field(body, "red");
    int i = 0;
    while (i < NLEADS && !(cJSON_IsString(red) && !strcmp(red->valuestring, LEAD_NAMES[i]))) i++;
    if (i == NLEADS) {
        err(e, "bad_request", "red must be one of unknown, none, input, ma, 10a");
        return NULL;
    }
    s_red_lead = i;
    meter_event_t ev = {.type = MEV_LEADS, .leads = i};
    meter_post_event(&ev);
    return bridge_leads();
}

// ------------------------------------------------------------------ operations
static bool require_connected(berr_t *e)
{
    if (meter_connected()) return true;
    err(e, "meter_disconnected", "meter is not connected");
    return false;
}

cJSON *bridge_info(void)
{
    meter_info_t i;
    meter_get_info(&i);
    cJSON *o = cJSON_CreateObject();
    add_str(o, "model", i.model, i.model[0]);
    add_str(o, "manufacturer", i.manufacturer, i.manufacturer[0]);
    add_str(o, "serial", i.serial, i.serial[0]);
    add_str(o, "meter_fw", i.meter_fw, i.meter_fw[0]);
    cJSON_AddStringToObject(o, "bridge_fw", bridge_version());
    cJSON_AddBoolToObject(o, "connected", meter_connected());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(o, "meter_vbus_fault", board_meter_vbus_fault());
    return o;
}

cJSON *bridge_state(berr_t *e)
{
    if (!require_connected(e)) return NULL;
    meter_state_t s;
    meter_get_state(&s);
    return state_json(&s);
}

// How long to wait for readings once a settle starts: base_ms plus per_reading_ms for each
// reading still to come. Every autorange step starts a new settle (about 1.8 s each), so a fixed
// deadline fails a long climb, e.g. res from 500 Ω to 50 MΩ on an open input, although the
// meter gets there.
typedef struct {
    int64_t base_ms, per_reading_ms;
} wait_t;

// A settle can push the deadline out by at most this much.
#define WAIT_EXTEND_MAX_MS 12000

// Collects n readings with seq > after_seq from subscription q. With ext, each settle that
// starts while waiting moves the deadline to ext's allowance from then, but never more than
// WAIT_EXTEND_MAX_MS past the original deadline. On failure the readings gathered so far go
// into the error's "readings" field.
static bool next_readings(QueueHandle_t q, uint32_t after_seq, int n, int64_t timeout_ms,
                          const wait_t *ext, meter_sample_t *out, berr_t *e)
{
    int got = 0;
    int64_t deadline = meter_millis() + timeout_ms;
    const int64_t limit = deadline + WAIT_EXTEND_MAX_MS;
    while (got < n) {
        int64_t left = deadline - meter_millis();
        meter_event_t ev;
        if (left <= 0 || xQueueReceive(q, &ev, pdMS_TO_TICKS(left)) != pdTRUE) {
            if (meter_millis() < deadline) continue;
            err(e, "meter_timeout", "got %d of %d readings before timeout", got, n);
            ESP_LOGW("bridge", "meter_timeout: got %d of %d readings after seq %lu (timeout %lld ms%s)", got,
                     n, (unsigned long)after_seq, timeout_ms, deadline > limit - WAIT_EXTEND_MAX_MS ? ", extended" : "");
            cJSON *a = cJSON_AddArrayToObject(extra(e), "readings");
            for (int i = 0; i < got; i++) cJSON_AddItemToArray(a, bridge_sample_json(&out[i], true));
            return false;
        }
        if (ev.type == MEV_STATUS && !ev.status.connected) {
            err(e, "meter_disconnected", "meter disconnected while waiting");
            return false;
        }
        if (ext && ev.type == MEV_STATE && !ev.state.settled) {
            int64_t want = meter_millis() + ext->base_ms + (n - got) * ext->per_reading_ms;
            if (want > deadline) deadline = want < limit ? want : limit;
        }
        if (ev.type == MEV_READING && ev.reading.seq > after_seq) out[got++] = ev.reading;
    }
    return true;
}

cJSON *bridge_reading(bool fresh, int timeout_ms, berr_t *e)
{
    if (!require_connected(e)) return NULL;
    meter_sample_t s;
    if (fresh) {
        QueueHandle_t q = meter_subscribe();
        if (!q) {
            err(e, "busy", "too many clients waiting for readings");
            return NULL;
        }
        bool ok = next_readings(q, meter_seq(), 1, timeout_ms, NULL, &s, e);
        meter_unsubscribe(q);
        return ok ? bridge_sample_json(&s, true) : NULL;
    }
    meter_state_t st;
    meter_get_state(&st);
    if (!st.settled || !meter_last_sample(&s)) {
        err(e, "busy", "meter is settling after a configuration change");
        cJSON_AddNumberToObject(extra(e), "retry_after_ms", METER_SETTLE_MAX_S * 1000);
        return NULL;
    }
    return bridge_sample_json(&s, true);
}

static void configure_failed(berr_t *e, const merr_t *m)
{
    err_from_meter(e, m);
    if (!strcmp(m->code, "verify_failed")) {
        meter_state_t st;
        meter_get_state(&st);
        cJSON_AddItemToObject(extra(e), "state", state_json(&st));
    }
}

static cJSON *changed_json(uint32_t ch)
{
    cJSON *a = cJSON_CreateArray();
    if (ch & CH_FUNCTION) cJSON_AddItemToArray(a, cJSON_CreateString("function"));
    if (ch & CH_RANGE) cJSON_AddItemToArray(a, cJSON_CreateString("range"));
    if (ch & CH_RATE) cJSON_AddItemToArray(a, cJSON_CreateString("rate"));
    if (ch & CH_BEEP) cJSON_AddItemToArray(a, cJSON_CreateString("beep"));
    return a;
}

cJSON *bridge_config(const cJSON *body, berr_t *e)
{
    if (!require_connected(e)) return NULL;
    meter_state_t cur;
    meter_get_state(&cur);
    meter_cfg_t c;
    bool force;
    if (!take_force(body, &force, e)) return NULL;
    if (!normalize_config(body, cur.function, CONFIG_KEYS, NKEYS(CONFIG_KEYS), &c, e)) return NULL;
    if (!force && !check_leads(&c, &cur, e)) return NULL;

    cJSON *out = NULL;
    xSemaphoreTake(op_lock, portMAX_DELAY);
    QueueHandle_t q = meter_subscribe();
    meter_cfg_result_t res;
    merr_t me = {0};
    meter_sample_t s;
    if (!q) {
        err(e, "busy", "too many clients waiting for readings");
    } else if (!meter_configure(&c, &res, &me)) {
        configure_failed(e, &me);
    } else {
        bool ok;
        const wait_t settle = {.base_ms = (int64_t)(METER_SETTLE_MAX_S * 1000) + 4000};
        if (res.settling || (res.changed & (CH_FUNCTION | CH_RANGE)))
            ok = next_readings(q, res.seq, 1, settle.base_ms, &settle, &s, e);
        else if (meter_last_sample(&s))
            ok = true;
        else
            ok = next_readings(q, res.seq, 1, 3000, NULL, &s, e);
        if (ok) {
            meter_state_t st;
            meter_get_state(&st);
            out = cJSON_CreateObject();
            cJSON_AddItemToObject(out, "changed", changed_json(res.changed));
            cJSON_AddItemToObject(out, "state", state_json(&st));
            cJSON_AddItemToObject(out, "reading", bridge_sample_json(&s, true));
        }
    }
    meter_unsubscribe(q);
    xSemaphoreGive(op_lock);
    return out;
}

cJSON *bridge_measure(const cJSON *body, berr_t *e)
{
    if (!require_connected(e)) return NULL;
    if (!cJSON_IsObject(body)) {
        err(e, "bad_request", "body must be a JSON object");
        return NULL;
    }
    const cJSON *sm = cJSON_GetObjectItemCaseSensitive(body, "samples");
    int samples = 1;
    if (sm) {
        double v = cJSON_IsNumber(sm) ? sm->valuedouble : 0;
        if (!cJSON_IsNumber(sm) || v != floor(v) || v < 1 || v > MAX_SAMPLES) {
            err(e, "bad_request", "samples must be an integer from 1 to %d", MAX_SAMPLES);
            return NULL;
        }
        samples = (int)v;
    }
    if (!check_unknown(body, MEASURE_KEYS, NKEYS(MEASURE_KEYS), e)) return NULL;
    const cJSON *tm = field(body, "timeout_ms");
    if (tm && (!cJSON_IsNumber(tm) || !(tm->valuedouble > 0))) {
        err(e, "bad_request", "timeout_ms must be a positive number");
        return NULL;
    }
    meter_state_t cur;
    meter_get_state(&cur);
    meter_cfg_t c;
    bool force;
    if (!take_force(body, &force, e)) return NULL;
    if (!normalize_config(body, cur.function, MEASURE_KEYS, NKEYS(MEASURE_KEYS), &c, e)) return NULL;
    if (!force && !check_leads(&c, &cur, e)) return NULL;

    meter_sample_t *rs = calloc(samples, sizeof *rs);
    if (!rs) {
        err(e, "internal", "out of memory");
        return NULL;
    }
    int64_t t0 = meter_millis();
    bool ok = false;
    xSemaphoreTake(op_lock, portMAX_DELAY);
    QueueHandle_t q = meter_subscribe();
    if (!q) {
        err(e, "busy", "too many clients waiting for readings");
    } else {
        uint32_t marker = meter_seq();
        meter_cfg_result_t res;
        merr_t me = {0};
        if (!meter_configure(&c, &res, &me)) {
            configure_failed(e, &me);
        } else {
            if (res.changed) marker = res.seq;
            meter_state_t st;
            meter_get_state(&st);
            double interval = PROTO_UPDATE_INTERVAL_S[st.rate >= 0 ? st.rate : 0];
            // An explicit timeout_ms is kept exactly; the default one follows the meter's settles.
            const wait_t wait = {.base_ms = (int64_t)((METER_SETTLE_MAX_S + 3) * 1000),
                                 .per_reading_ms = (int64_t)(interval * 2 * 1000)};
            int64_t timeout_ms = tm ? (int64_t)tm->valuedouble : wait.base_ms + samples * wait.per_reading_ms;
            ok = next_readings(q, marker, samples, timeout_ms, tm ? NULL : &wait, rs, e);
        }
    }
    meter_unsubscribe(q);
    xSemaphoreGive(op_lock);
    if (!ok) {
        free(rs);
        return NULL;
    }

    cJSON *out = cJSON_CreateObject();
    add_str(out, "function", rs[0].function >= 0 ? PROTO_FUNCS[rs[0].function].name : "", rs[0].function >= 0);
    add_str(out, "unit", rs[0].unit, rs[0].unit[0] != 0);
    add_str(out, "range_label", rs[samples - 1].range_label, rs[samples - 1].range_label_valid);
    cJSON *arr = cJSON_AddArrayToObject(out, "samples");
    int nv = 0;
    double sum = 0, mn = INFINITY, mx = -INFINITY;
    for (int i = 0; i < samples; i++) {
        if (rs[i].overload) {
            cJSON_AddItemToArray(arr, cJSON_CreateNull());
            continue;
        }
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(rs[i].value));
        nv++;
        sum += rs[i].value;
        mn = fmin(mn, rs[i].value);
        mx = fmax(mx, rs[i].value);
    }
    cJSON_AddBoolToObject(out, "overload", nv < samples);
    if (nv) {
        double mean = sum / nv, ss = 0;
        for (int i = 0; i < samples; i++)
            if (!rs[i].overload) ss += (rs[i].value - mean) * (rs[i].value - mean);
        cJSON_AddNumberToObject(out, "mean", mean);
        cJSON_AddNumberToObject(out, "min", mn);
        cJSON_AddNumberToObject(out, "max", mx);
        cJSON_AddNumberToObject(out, "stdev", nv > 1 ? sqrt(ss / (nv - 1)) : 0.0);  // sample stdev
    } else {
        cJSON_AddNullToObject(out, "mean");
        cJSON_AddNullToObject(out, "min");
        cJSON_AddNullToObject(out, "max");
        cJSON_AddNullToObject(out, "stdev");
    }
    cJSON_AddNumberToObject(out, "duration_ms", (double)(meter_millis() - t0));
    free(rs);
    return out;
}

cJSON *bridge_scpi(const cJSON *cmd, berr_t *e)
{
    if (!require_connected(e)) return NULL;
    char buf[208];
    const char *s = cJSON_IsString(cmd) ? cmd->valuestring : NULL;
    size_t n = s ? strlen(s) : 0;
    if (s) {
        while (n && isspace((unsigned char)s[n - 1])) n--;
        while (n && isspace((unsigned char)*s)) s++, n--;
    }
    if (!s || !n || n > 200 || memchr(s, '\n', n)) {
        err(e, "bad_request", "cmd must be a single-line SCPI command");
        return NULL;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    char reply[128];
    bool has_reply = false;
    merr_t me = {0};
    xSemaphoreTake(op_lock, portMAX_DELAY);
    bool ok = meter_scpi(buf, reply, sizeof reply, &has_reply, &me);
    xSemaphoreGive(op_lock);
    if (!ok) {
        err_from_meter(e, &me);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    add_str(out, "reply", reply, has_reply);
    return out;
}
