#include "meter.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "proto.h"
#include "usb_link.h"

static const char *TAG = "meter";

#define QUERY_TIMEOUT_MS 600        // unsupported commands never reply; 600 ms is enough
#define READY_PROBE_TIMEOUT_MS 80
#define POLL_PERIOD_MS 100          // MEAS?/RANGE?/MEAS:SHOW? polling
#define STATE_POLL_PERIOD_MS 1000   // FUNC?/AUTO?/RATE? polling (front-panel changes)
// After a function or range change the meter reports stale values (the previous raw number
// rescaled into the new unit) for about 2 to 2.8 s. A reading counts as real once the value
// changes at least SETTLE_MIN_MS after the change, or after SETTLE_MAX_MS regardless.
#define SETTLE_MIN_MS 1500
#define SETTLE_MAX_MS ((int64_t)(METER_SETTLE_MAX_S * 1000))
#define OVERLOAD_THRESHOLD 9.9e8    // the meter reports 1E+9 for overload/open
#define MISSES_BEFORE_OFFLINE 3
#define OPEN_WAIT_MS 2000
// The meter's CH340 is powered from our VBUS switch: if the port is open but the meter has not
// answered for this long, switch VBUS off and on (at most once per VBUS_CYCLE_GAP_MS).
#define VBUS_CYCLE_AFTER_MS 30000
#define VBUS_CYCLE_GAP_MS 300000
#define MAX_SUBS 10
#define SUB_DEPTH 16

// ------------------------------------------------------------------ meter-task-private state
static meter_state_t st;
static meter_info_t info;
static bool port_open, responding;
static uint32_t seq;
static meter_sample_t last;
static bool have_last;
static int misses;
static char last_raw[32];
static int64_t last_emit_ms;
static int64_t settle_start_ms = -1;
static int64_t next_state_poll_ms;
static int64_t unresponsive_since_ms = -1;
static int64_t last_vbus_cycle_ms = -VBUS_CYCLE_GAP_MS;
static volatile bool vbus_cycle_requested;

// ------------------------------------------------------------------ copies other tasks read
static SemaphoreHandle_t pub_lock;
static meter_state_t pub_state;
static meter_info_t pub_info;
static bool pub_port_open, pub_responding, pub_have_last;
static uint32_t pub_seq;
static meter_sample_t pub_last;

static SemaphoreHandle_t subs_lock;
static QueueHandle_t subs[MAX_SUBS];

typedef enum { JOB_CONFIG, JOB_SCPI } job_kind_t;
typedef struct {
    job_kind_t kind;
    const meter_cfg_t *cfg;
    meter_cfg_result_t *res;
    const char *cmd;
    char *reply;
    size_t reply_len;
    bool *has_reply;
    merr_t *err;
    bool ok;
    SemaphoreHandle_t done;
} job_t;
static QueueHandle_t jobs;

int64_t meter_millis(void) { return esp_timer_get_time() / 1000; }

static void set_err(merr_t *e, const char *code, const char *fmt, ...)
{
    if (!e) return;
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof e->message, fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------------------ publishing and events
static void publish(void)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    pub_state = st;
    pub_info = info;
    pub_port_open = port_open;
    pub_responding = responding;
    pub_seq = seq;
    pub_last = last;
    pub_have_last = have_last;
    xSemaphoreGive(pub_lock);
}

static void dispatch(const meter_event_t *ev);
void meter_post_event(const meter_event_t *ev) { dispatch(ev); }

static void dispatch(const meter_event_t *ev)
{
    xSemaphoreTake(subs_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!subs[i]) continue;
        if (xQueueSend(subs[i], ev, 0) != pdTRUE) {
            meter_event_t junk;  // slow consumer: drop its oldest event rather than block
            xQueueReceive(subs[i], &junk, 0);
            xQueueSend(subs[i], ev, 0);
        }
    }
    xSemaphoreGive(subs_lock);
}

static void emit_state(void)
{
    publish();
    meter_event_t ev = {.type = MEV_STATE};
    ev.state = st;
    dispatch(&ev);
}

static void emit_status(void)
{
    publish();
    meter_event_t ev = {.type = MEV_STATUS};
    ev.status.connected = port_open && responding;
    ev.status.port_open = port_open;
    dispatch(&ev);
}

// ------------------------------------------------------------------ serial primitives
static void strip(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static bool query_raw(const char *cmd, uint32_t timeout_ms, char *out, size_t len)
{
    link_reset_input();
    if (!link_write_line(cmd)) return false;
    return link_read_line(out, len, timeout_ms) >= 0;
}

// Query and decode. RANGE? sends UTF-8 but MEAS:SHOW? sends GB2312, so anything that is not
// valid UTF-8 is decoded as GB2312.
static bool query(const char *cmd, uint32_t timeout_ms, char *out, size_t len)
{
    char raw[128];
    if (!query_raw(cmd, timeout_ms, raw, sizeof raw)) return false;
    if (proto_utf8_valid(raw)) snprintf(out, len, "%s", raw);
    else proto_gb2312_to_utf8(raw, out, len);
    strip(out);
    return true;
}

// The meter ignores most commands while busy. Probe with a cheap query until it answers. While
// busy it sometimes answers a probe late instead of dropping it; a late reply would be taken
// for the answer to the next query (an AUTO? "1" read as MEAS? is a phantom 1 V reading), so
// drain any trailing replies before returning.
static bool wait_ready(uint32_t max_ms, merr_t *e)
{
    int64_t t0 = meter_millis();
    char buf[32];
    while (meter_millis() - t0 < max_ms) {
        if (query_raw("AUTO?", READY_PROBE_TIMEOUT_MS, buf, sizeof buf)) {
            link_drain(100);
            return true;
        }
        if (!link_is_open()) break;
    }
    if (!link_is_open()) set_err(e, "meter_disconnected", "lost USB connection to meter");
    else set_err(e, "meter_timeout", "meter still busy after %.1f s", max_ms / 1000.0);
    return false;
}

static bool send(const char *cmd, uint32_t max_ms, merr_t *e)
{
    if (!link_write_line(cmd)) {
        set_err(e, "meter_disconnected", "lost USB connection to meter");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    return wait_ready(max_ms, e);
}

// ------------------------------------------------------------------ state
static void begin_settle(void)
{
    settle_start_ms = meter_millis();
    st.settled = false;
    emit_state();
}

static void set_range_label(const char *label)
{
    st.range_label_valid = true;
    snprintf(st.range_label, sizeof st.range_label, "%s", label);
    st.range_valid = proto_parse_range(label, &st.range);
}

// Queries the configuration; begins a settle when the function or range changed.
static void refresh_state(bool full)
{
    char buf[64];
    bool settle = false, changed = false;
    if (query("FUNC?", QUERY_TIMEOUT_MS, buf, sizeof buf)) {
        int f = proto_func_by_reply(buf);
        if (f >= 0 && f != st.function) {
            st.function = f;
            settle = changed = true;
        }
    }
    if (query("RANGE?", QUERY_TIMEOUT_MS, buf, sizeof buf) && proto_valid_range_label(buf)) {
        if (!st.range_label_valid || strcmp(buf, st.range_label) != 0) {
            set_range_label(buf);
            settle = changed = true;
        }
    }
    if (query("AUTO?", QUERY_TIMEOUT_MS, buf, sizeof buf) && (!strcmp(buf, "0") || !strcmp(buf, "1"))) {
        int a = buf[0] == '1';
        if (a != st.autorange) st.autorange = a, changed = true;
    }
    if (query("RATE?", QUERY_TIMEOUT_MS, buf, sizeof buf)) {
        int r = proto_rate_by_code(buf);
        if (r >= 0 && r != st.rate) st.rate = r, changed = true;
    }
    if (full) {
        if (query("SYST:BEEP?", 1000, buf, sizeof buf) && (!strcmp(buf, "ON") || !strcmp(buf, "OFF"))) {
            int b = !strcmp(buf, "ON");
            if (b != st.beep) st.beep = b, changed = true;
        }
        if (query("TEMP:RTD:UNIT?", QUERY_TIMEOUT_MS, buf, sizeof buf) && buf[0]) {
            if (strcmp(buf, st.temp_unit)) snprintf(st.temp_unit, sizeof st.temp_unit, "%s", buf), changed = true;
        }
    }
    if (settle && !full) begin_settle();
    else if (changed) emit_state();
}

static int64_t interval_ms(void)
{
    return (int64_t)(PROTO_UPDATE_INTERVAL_S[st.rate >= 0 ? st.rate : 0] * 1000);
}

// ------------------------------------------------------------------ open / close
static void close_link(void)
{
    link_close();
    if (port_open || responding) {
        port_open = responding = false;
        st.settled = false;
        emit_status();
    }
}

static void split_field(const char **p, char *out, size_t len)
{
    const char *s = *p;
    const char *c = s ? strchr(s, ',') : NULL;
    size_t n = s ? (c ? (size_t)(c - s) : strlen(s)) : 0;
    if (n >= len) n = len - 1;
    if (s) memcpy(out, s, n);
    out[n] = 0;
    strip(out);
    *p = c ? c + 1 : NULL;
}

// Meter (re)appeared: read identity and full state.
static void on_responding(void)
{
    char idn[96];
    if (!query("*IDN?", QUERY_TIMEOUT_MS, idn, sizeof idn)) {
        responding = false;
        return;
    }
    const char *p = idn;
    snprintf(info.idn, sizeof info.idn, "%s", idn);
    split_field(&p, info.manufacturer, sizeof info.manufacturer);
    split_field(&p, info.model, sizeof info.model);
    split_field(&p, info.serial, sizeof info.serial);
    split_field(&p, info.meter_fw, sizeof info.meter_fw);
    ESP_LOGI(TAG, "meter: %s", idn);
    responding = true;
    misses = 0;
    unresponsive_since_ms = -1;
    refresh_state(true);
    st.settled = true;
    emit_status();
    emit_state();
}

static bool open_link(void)
{
    if (!link_open(OPEN_WAIT_MS)) return false;
    port_open = true;
    unresponsive_since_ms = meter_millis();
    emit_status();
    on_responding();
    return true;
}

static void vbus_cycle(void)
{
    ESP_LOGW(TAG, "power-cycling the meter's VBUS");
    close_link();
    board_meter_vbus(false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    board_meter_vbus(true);
    last_vbus_cycle_ms = meter_millis();
    unresponsive_since_ms = -1;
}

// ------------------------------------------------------------------ polling
static void emit_sample(const char *raw, const char *display)
{
    double value = strtod(raw, NULL);
    meter_sample_t s = {0};
    s.overload = isinf(value) || fabs(value) >= OVERLOAD_THRESHOLD;
    s.value = s.overload ? 0 : value;
    s.function = st.function;
    if (st.function >= 0) {
        const char *unit = PROTO_FUNCS[st.function].unit;
        if (!strcmp(PROTO_FUNCS[st.function].name, "temp") && st.temp_unit[0]) unit = st.temp_unit;
        snprintf(s.unit, sizeof s.unit, "%s", unit);
    }
    s.range_label_valid = st.range_label_valid;
    snprintf(s.range_label, sizeof s.range_label, "%s", st.range_label);
    s.display_valid = display != NULL;
    if (display) snprintf(s.display, sizeof s.display, "%s", display);
    s.seq = ++seq;
    s.t_ms = meter_millis();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s.ts_ms = tv.tv_sec > 1700000000 ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
    last_emit_ms = s.t_ms;
    last = s;
    have_last = true;
    publish();
    meter_event_t ev = {.type = MEV_READING};
    ev.reading = s;
    dispatch(&ev);
}

static void poll(void)
{
    int64_t now = meter_millis();
    if (!responding) {
        if (now >= next_state_poll_ms) {
            next_state_poll_ms = now + 2000;
            on_responding();
        }
        return;
    }
    char meas[32], rng[32], display[48];
    if (!query("MEAS?", QUERY_TIMEOUT_MS, meas, sizeof meas)) {
        ESP_LOGI(TAG, "MEAS? no reply (miss %d)", misses + 1);
        if (++misses >= MISSES_BEFORE_OFFLINE) {
            ESP_LOGW(TAG, "meter stopped answering");
            responding = false;
            st.settled = false;
            unresponsive_since_ms = now;
            emit_status();
        }
        return;
    }
    misses = 0;
    if (!proto_valid_measurement(meas)) {
        ESP_LOGI(TAG, "MEAS? bad reply \"%s\": resync", meas);
        link_drain(100);  // out of step with the meter; resynchronise
        return;
    }
    bool have_rng = query("RANGE?", QUERY_TIMEOUT_MS, rng, sizeof rng);
    bool have_display = query("MEAS:SHOW?", QUERY_TIMEOUT_MS, display, sizeof display);
    if (have_rng && proto_valid_range_label(rng) && (!st.range_label_valid || strcmp(rng, st.range_label))) {
        // Front-panel change or an autorange step: readings are stale for a while.
        ESP_LOGI(TAG, "range %s -> %s: settle", st.range_label_valid ? st.range_label : "?", rng);
        set_range_label(rng);
        begin_settle();
    }
    if (now >= next_state_poll_ms) {
        next_state_poll_ms = now + STATE_POLL_PERIOD_MS;
        refresh_state(false);
    }

    bool changed = strcmp(meas, last_raw) != 0;
    snprintf(last_raw, sizeof last_raw, "%s", meas);

    if (settle_start_ms >= 0) {
        int64_t elapsed = now - settle_start_ms;
        if ((changed && elapsed >= SETTLE_MIN_MS) || elapsed >= SETTLE_MAX_MS) {
            ESP_LOGI(TAG, "settled after %lld ms (%s)", elapsed, changed ? "value changed" : "max wait");
            settle_start_ms = -1;
            st.settled = true;
            emit_state();
            emit_sample(meas, have_display ? display : NULL);
        }
        return;
    }
    // A stable input repeats the same value, so also emit once per update interval.
    if (changed || now - last_emit_ms >= interval_ms() * 3 / 2)
        emit_sample(meas, have_display ? display : NULL);
}

// ------------------------------------------------------------------ jobs
static bool isclose(double a, double b) { return fabs(a - b) <= 1e-6 * fmax(fabs(a), fabs(b)); }

static bool func_is(int f)
{
    char buf[32];
    return query("FUNC?", QUERY_TIMEOUT_MS, buf, sizeof buf) && proto_func_by_reply(buf) == f;
}

static bool reply_is(const char *cmd, uint32_t timeout_ms, const char *want)
{
    char buf[32];
    return query(cmd, timeout_ms, buf, sizeof buf) && !strcmp(buf, want);
}

static bool range_is(double full_scale)
{
    refresh_state(false);
    return st.autorange == 0 && st.range_valid && isclose(st.range, full_scale);
}

#define TRY(x) do { if (!(x)) return false; } while (0)

static bool do_configure(const meter_cfg_t *c, meter_cfg_result_t *r, merr_t *e)
{
    refresh_state(false);
    uint32_t changed = 0;

    if (c->function >= 0 && c->function != st.function) {
        const char *cmd = PROTO_FUNCS[c->function].conf_cmd;
        TRY(send(cmd, 2000, e));
        if (!func_is(c->function)) {
            TRY(send(cmd, 2000, e));
            if (!func_is(c->function)) {
                set_err(e, "verify_failed", "meter did not accept function %s", PROTO_FUNCS[c->function].name);
                return false;
            }
        }
        refresh_state(false);
        begin_settle();
        changed |= CH_FUNCTION;
    }

    int target = c->function >= 0 ? c->function : st.function;
    if (c->range_kind == RANGE_AUTO) {
        if (st.autorange != 1) {
            TRY(send("AUTO", 1000, e));
            if (!reply_is("AUTO?", QUERY_TIMEOUT_MS, "1")) {
                TRY(send("AUTO", 1000, e));
                if (!reply_is("AUTO?", QUERY_TIMEOUT_MS, "1")) {
                    set_err(e, "verify_failed", "meter did not accept autorange");
                    return false;
                }
            }
            st.autorange = 1;
            changed |= CH_RANGE;
        }
    } else if (c->range_kind == RANGE_VALUE) {
        double fs = 0;
        int index = target >= 0 ? proto_select_range(target, c->range, &fs) : 0;
        if (!index) {
            set_err(e, "bad_request", "no %s range for %g", target >= 0 ? PROTO_FUNCS[target].name : "unknown function", c->range);
            return false;
        }
        bool same = st.autorange == 0 && st.range_valid && isclose(st.range, fs);
        if (!same) {
            // RANGE <n> is fast (about 55 ms) and does not reconfigure the function.
            char cmd[16];
            snprintf(cmd, sizeof cmd, "RANGE %d", index);
            TRY(send(cmd, 1000, e));
            if (!range_is(fs)) {
                TRY(send(cmd, 1000, e));
                if (!range_is(fs)) {
                    set_err(e, "verify_failed", "meter did not accept range %g", fs);
                    return false;
                }
            }
            begin_settle();
            changed |= CH_RANGE;
        }
    }

    if (c->rate >= 0 && c->rate != st.rate) {
        char cmd[16];
        snprintf(cmd, sizeof cmd, "RATE %s", PROTO_RATE_CODES[c->rate]);
        TRY(send(cmd, 2000, e));
        if (!reply_is("RATE?", QUERY_TIMEOUT_MS, PROTO_RATE_CODES[c->rate])) {
            TRY(send(cmd, 2000, e));
            if (!reply_is("RATE?", QUERY_TIMEOUT_MS, PROTO_RATE_CODES[c->rate])) {
                set_err(e, "verify_failed", "meter did not accept rate %s", PROTO_RATE_NAMES[c->rate]);
                return false;
            }
        }
        st.rate = c->rate;
        changed |= CH_RATE;
    }

    if (c->beep >= 0 && c->beep != st.beep) {
        const char *want = c->beep ? "ON" : "OFF";
        char cmd[20];
        snprintf(cmd, sizeof cmd, "SYST:BEEP %s", want);
        TRY(send(cmd, 2000, e));
        if (!reply_is("SYST:BEEP?", 1000, want)) {
            TRY(send(cmd, 2000, e));
            if (!reply_is("SYST:BEEP?", 1000, want)) {
                set_err(e, "verify_failed", "meter did not accept beep %s", want);
                return false;
            }
        }
        st.beep = c->beep;
        changed |= CH_BEEP;
    }

    if (changed) emit_state();
    r->changed = changed;
    r->seq = seq;
    r->settling = settle_start_ms >= 0;
    return true;
}

static bool do_scpi(const char *cmd, char *reply, size_t len, bool *has_reply, merr_t *e)
{
    *has_reply = false;
    if (strchr(cmd, '?')) {
        *has_reply = query(cmd, 1000, reply, len);
    } else {
        TRY(send(cmd, 2000, e));
    }
    refresh_state(strncasecmp(cmd, "SYST", 4) == 0);
    return true;
}

static void run_job(job_t *j)
{
    if (!responding) {
        set_err(j->err, "meter_disconnected", "meter is not responding");
        j->ok = false;
    } else if (j->kind == JOB_CONFIG) {
        ESP_LOGI(TAG, "configure: function %s", j->cfg->function >= 0 ? PROTO_FUNCS[j->cfg->function].name : "-");
        j->ok = do_configure(j->cfg, j->res, j->err);
        ESP_LOGI(TAG, "configure %s (seq %lu)", j->ok ? "done" : "failed", (unsigned long)seq);
    } else {
        j->ok = do_scpi(j->cmd, j->reply, j->reply_len, j->has_reply, j->err);
    }
    if (!j->ok && !link_is_open()) set_err(j->err, "meter_disconnected", "lost USB connection to meter");
}

static void fail_pending(const char *code, const char *msg)
{
    job_t *j;
    while (xQueueReceive(jobs, &j, 0) == pdTRUE) {
        set_err(j->err, code, "%s", msg);
        j->ok = false;
        xSemaphoreGive(j->done);
    }
}

// ------------------------------------------------------------------ task
static void meter_task(void *arg)
{
    int64_t next_poll = 0;
    for (;;) {
        if (!link_is_open()) {
            close_link();
            fail_pending("meter_disconnected", "meter USB port is not open");
            if (vbus_cycle_requested) {
                vbus_cycle_requested = false;
                vbus_cycle();
            }
            if (!open_link()) {
                fail_pending("meter_disconnected", "meter USB port is not open");
                continue;  // link_open already waited OPEN_WAIT_MS
            }
            next_poll = 0;
        }
        int64_t now = meter_millis();
        if (port_open && !responding && unresponsive_since_ms < 0) unresponsive_since_ms = now;
        if (vbus_cycle_requested ||
            (port_open && !responding && unresponsive_since_ms >= 0 &&
             now - unresponsive_since_ms > VBUS_CYCLE_AFTER_MS && now - last_vbus_cycle_ms > VBUS_CYCLE_GAP_MS)) {
            vbus_cycle_requested = false;
            vbus_cycle();
            continue;
        }
        int64_t wait = next_poll - now;
        job_t *j;
        if (xQueueReceive(jobs, &j, wait > 0 ? pdMS_TO_TICKS(wait) : 0) == pdTRUE) {
            run_job(j);
            publish();
            xSemaphoreGive(j->done);
            continue;
        }
        next_poll = meter_millis() + POLL_PERIOD_MS;
        poll();
        publish();
    }
}

void meter_start(void)
{
    st.function = st.autorange = st.rate = st.beep = -1;
    pub_lock = xSemaphoreCreateMutex();
    subs_lock = xSemaphoreCreateMutex();
    jobs = xQueueCreate(8, sizeof(job_t *));
    publish();
    link_init();
    xTaskCreate(meter_task, "meter", 6144, NULL, 6, NULL);
}

// ------------------------------------------------------------------ public API
bool meter_connected(void)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    bool c = pub_port_open && pub_responding;
    xSemaphoreGive(pub_lock);
    return c;
}

bool meter_port_open(void)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    bool c = pub_port_open;
    xSemaphoreGive(pub_lock);
    return c;
}

void meter_get_state(meter_state_t *out)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    *out = pub_state;
    xSemaphoreGive(pub_lock);
}

bool meter_last_sample(meter_sample_t *out)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    bool have = pub_have_last;
    if (have) *out = pub_last;
    xSemaphoreGive(pub_lock);
    return have;
}

uint32_t meter_seq(void)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    uint32_t s = pub_seq;
    xSemaphoreGive(pub_lock);
    return s;
}

void meter_get_info(meter_info_t *out)
{
    xSemaphoreTake(pub_lock, portMAX_DELAY);
    *out = pub_info;
    xSemaphoreGive(pub_lock);
}

QueueHandle_t meter_subscribe(void)
{
    QueueHandle_t q = xQueueCreate(SUB_DEPTH, sizeof(meter_event_t));
    if (!q) return NULL;
    xSemaphoreTake(subs_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!subs[i]) {
            subs[i] = q;
            xSemaphoreGive(subs_lock);
            return q;
        }
    }
    xSemaphoreGive(subs_lock);
    vQueueDelete(q);
    return NULL;
}

void meter_unsubscribe(QueueHandle_t q)
{
    if (!q) return;
    xSemaphoreTake(subs_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i] == q) subs[i] = NULL;
    xSemaphoreGive(subs_lock);
    vQueueDelete(q);
}

static bool submit(job_t *j)
{
    j->done = xSemaphoreCreateBinary();
    if (!j->done) {
        set_err(j->err, "internal", "out of memory");
        return false;
    }
    xQueueSend(jobs, &j, portMAX_DELAY);
    xSemaphoreTake(j->done, portMAX_DELAY);  // every job finishes: each step has a timeout
    vSemaphoreDelete(j->done);
    return j->ok;
}

bool meter_configure(const meter_cfg_t *cfg, meter_cfg_result_t *res, merr_t *err)
{
    job_t j = {.kind = JOB_CONFIG, .cfg = cfg, .res = res, .err = err};
    return submit(&j);
}

bool meter_scpi(const char *cmd, char *reply, size_t reply_len, bool *has_reply, merr_t *err)
{
    job_t j = {.kind = JOB_SCPI, .cmd = cmd, .reply = reply, .reply_len = reply_len, .has_reply = has_reply, .err = err};
    return submit(&j);
}

void meter_request_vbus_cycle(void) { vbus_cycle_requested = true; }
