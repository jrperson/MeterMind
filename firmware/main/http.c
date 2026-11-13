// Transports on one port: REST, WebSocket /ws, MCP /mcp and the dashboard. Port of
// bridge/server.py. esp_http_server runs every handler on one task, so anything that can wait
// on the meter (config, measure, scpi, fresh readings, MCP) is handed to a small worker pool
// with httpd_req_async_handler_begin; quick reads are answered inline. In setup mode "/" is the
// WiFi setup page and unknown paths redirect to it (portal.c).
#include "http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bridge.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mcp.h"
#include "net.h"
#include "portal.h"
#include "sdkconfig.h"

static const char *TAG = "http";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define MAX_BODY 8192
#define MAX_WS_MSG 4096
#define NWORKERS 3
#define WORK_DEPTH 8
#define MAX_WS 6

typedef enum { W_READING, W_CONFIG, W_MEASURE, W_SCPI, W_MCP, W_WS_OP } work_kind_t;
typedef struct {
    work_kind_t kind;
    httpd_req_t *req;  // async copy (HTTP work)
    int fd;            // WebSocket work
    char *body;        // owned, may be NULL
    bool fresh;
    int timeout_ms;
} work_t;

static httpd_handle_t s_server;
static QueueHandle_t s_work;
static QueueHandle_t s_hello;           // WebSocket fds waiting for their hello message
static SemaphoreHandle_t s_ws_lock;     // guards the client list and serialises frame sends
static int s_ws_fds[MAX_WS];

// ------------------------------------------------------------------ responses
static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 502: return "502 Bad Gateway";
    case 503: return "503 Service Unavailable";
    case 504: return "504 Gateway Timeout";
    default: return "500 Internal Server Error";
    }
}

static esp_err_t send_json(httpd_req_t *req, int status, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, s ? s : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(s);
    return r;
}

static esp_err_t send_error(httpd_req_t *req, berr_t *e)
{
    return send_json(req, bridge_http_status(e->code), bridge_error_body(e));
}

static esp_err_t send_simple_error(httpd_req_t *req, const char *code, const char *message)
{
    berr_t e = {0};
    snprintf(e.code, sizeof e.code, "%s", code);
    snprintf(e.message, sizeof e.message, "%s", message);
    return send_error(req, &e);
}

// DNS-rebinding protection (required by the MCP spec, sensible for the whole API): a browser
// page from another site may not drive the meter. Non-browser clients send no Origin.
static bool origin_ok(httpd_req_t *req)
{
    char origin[128], host[96] = "";
    esp_err_t r = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin);
    if (r == ESP_ERR_NOT_FOUND) return true;
    if (r != ESP_OK) return false;
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    char *netloc = strstr(origin, "://");
    netloc = netloc ? netloc + 3 : origin;
    char *slash = strchr(netloc, '/');
    if (slash) *slash = 0;
    if (!strcmp(netloc, host)) return true;
    char hostname[64];
    const char *h = netloc;
    size_t n;
    if (*h == '[') {
        h++;
        const char *end = strchr(h, ']');
        n = end ? (size_t)(end - h) : strlen(h);
    } else {
        const char *colon = strchr(h, ':');
        n = colon ? (size_t)(colon - h) : strlen(h);
    }
    if (n >= sizeof hostname) return false;
    memcpy(hostname, h, n);
    hostname[n] = 0;
    return !strcasecmp(hostname, "localhost") || !strcmp(hostname, "127.0.0.1");
}

static esp_err_t forbidden(httpd_req_t *req)
{
    return send_simple_error(req, "forbidden", "cross-origin request refused");
}

bool http_origin_ok(httpd_req_t *req) { return origin_ok(req); }
esp_err_t http_send_json(httpd_req_t *req, int status, cJSON *obj) { return send_json(req, status, obj); }
esp_err_t http_send_error(httpd_req_t *req, const char *code, const char *message)
{
    return send_simple_error(req, code, message);
}

// Reads the request body (NULL when there is none). False with *e filled on failure.
static bool read_body(httpd_req_t *req, char **out, berr_t *e)
{
    *out = NULL;
    size_t len = req->content_len;
    if (!len) return true;
    if (len > MAX_BODY) {
        snprintf(e->code, sizeof e->code, "bad_request");
        snprintf(e->message, sizeof e->message, "body too large (max %d bytes)", MAX_BODY);
        return false;
    }
    char *buf = malloc(len + 1);
    size_t got = 0;
    while (buf && got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    if (!buf || got < len) {
        free(buf);
        snprintf(e->code, sizeof e->code, "bad_request");
        snprintf(e->message, sizeof e->message, "could not read the request body");
        return false;
    }
    buf[len] = 0;
    *out = buf;
    return true;
}

static cJSON *parse_body(const char *body, berr_t *e)
{
    if (!body) return cJSON_CreateObject();
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        snprintf(e->code, sizeof e->code, "bad_request");
        snprintf(e->message, sizeof e->message, "body is not valid JSON");
    }
    return j;
}

// ------------------------------------------------------------------ async hand-off
static esp_err_t go_async(httpd_req_t *req, work_kind_t kind, bool fresh, int timeout_ms)
{
    berr_t e = {0};
    char *body = NULL;
    if (kind != W_READING && !read_body(req, &body, &e)) {
        if (kind == W_MCP) {
            cJSON *r = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
            return send_json(req, 400, r);
        }
        return send_error(req, &e);
    }
    httpd_req_t *copy;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        free(body);
        return send_simple_error(req, "internal", "could not start the request");
    }
    work_t w = {.kind = kind, .req = copy, .body = body, .fresh = fresh, .timeout_ms = timeout_ms};
    if (xQueueSend(s_work, &w, 0) != pdTRUE) {
        free(body);
        send_simple_error(copy, "busy", "too many requests in flight");
        httpd_req_async_handler_complete(copy);
    }
    return ESP_OK;
}

static void mcp_post(httpd_req_t *req, const char *body)
{
    cJSON *payload = body ? cJSON_Parse(body) : NULL;
    if (!payload) {
        send_json(req, 400,
                  cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}"));
        return;
    }
    cJSON *reply = NULL;
    if (cJSON_IsArray(payload)) {  // batches (older protocol versions)
        cJSON *arr = cJSON_CreateArray();
        for (cJSON *m = payload->child; m; m = m->next) {
            cJSON *r = mcp_handle(m);
            if (r) cJSON_AddItemToArray(arr, r);
        }
        if (arr->child) reply = arr;
        else cJSON_Delete(arr);
    } else {
        reply = mcp_handle(payload);
    }
    cJSON_Delete(payload);
    if (reply) {
        send_json(req, 200, reply);
    } else {
        httpd_resp_set_status(req, status_line(202));
        httpd_resp_send(req, NULL, 0);
    }
}

static void http_work(work_t *w)
{
    berr_t e = {0};
    cJSON *res = NULL;
    if (w->kind == W_MCP) {
        mcp_post(w->req, w->body);
        return;
    }
    if (w->kind == W_READING) {
        res = bridge_reading(w->fresh, w->timeout_ms, &e);
    } else {
        cJSON *body = parse_body(w->body, &e);
        if (body && w->kind == W_CONFIG) res = bridge_config(body, &e);
        else if (body && w->kind == W_MEASURE) res = bridge_measure(body, &e);
        else if (body && w->kind == W_SCPI) {
            if (cJSON_IsObject(body)) {
                res = bridge_scpi(cJSON_GetObjectItemCaseSensitive(body, "cmd"), &e);
            } else {
                snprintf(e.code, sizeof e.code, "bad_request");
                snprintf(e.message, sizeof e.message, "body must be a JSON object");
            }
        }
        cJSON_Delete(body);
    }
    if (res) send_json(w->req, 200, res);
    else send_error(w->req, &e);
}

// ------------------------------------------------------------------ WebSocket
static void ws_add(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS; i++)
        if (!s_ws_fds[i]) {
            s_ws_fds[i] = fd;
            break;
        }
    xSemaphoreGive(s_ws_lock);
}

static void ws_remove(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS; i++)
        if (s_ws_fds[i] == fd) s_ws_fds[i] = 0;
    xSemaphoreGive(s_ws_lock);
}

static bool ws_send(int fd, const char *text)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    bool ok = false;
    if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text)};
        ok = httpd_ws_send_frame_async(s_server, fd, &f) == ESP_OK;
    }
    xSemaphoreGive(s_ws_lock);
    if (!ok) httpd_sess_trigger_close(s_server, fd);
    return ok;
}

static void ws_send_json(int fd, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s) ws_send(fd, s);
    cJSON_free(s);
}

static void ws_hello(int fd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddItemToObject(o, "info", bridge_info());
    cJSON_AddItemToObject(o, "leads", bridge_leads());
    if (meter_connected()) {
        berr_t e = {0};
        cJSON *st = bridge_state(&e);
        if (st) cJSON_AddItemToObject(o, "state", st);
        bridge_err_free(&e);
        meter_sample_t s;
        if (meter_last_sample(&s)) cJSON_AddItemToObject(o, "reading", bridge_sample_json(&s, false));
    }
    ws_send_json(fd, o);
}

static void ws_op(int fd, const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    if (!msg) return;  // validated in ws_handler
    cJSON *id = cJSON_DetachItemFromObjectCaseSensitive(msg, "id");
    cJSON *opj = cJSON_DetachItemFromObjectCaseSensitive(msg, "op");
    const char *op = cJSON_IsString(opj) ? opj->valuestring : "None";
    berr_t e = {0};
    cJSON *res = NULL;
    if (!strcmp(op, "config")) res = bridge_config(msg, &e);
    else if (!strcmp(op, "measure")) res = bridge_measure(msg, &e);
    else if (!strcmp(op, "reading")) {
        const cJSON *fresh = cJSON_GetObjectItemCaseSensitive(msg, "fresh");
        const cJSON *tmo = cJSON_GetObjectItemCaseSensitive(msg, "timeout_ms");
        bool f = cJSON_IsTrue(fresh) || (cJSON_IsNumber(fresh) && fresh->valuedouble != 0) ||
                 (cJSON_IsString(fresh) && fresh->valuestring[0]);
        res = bridge_reading(f, cJSON_IsNumber(tmo) ? (int)tmo->valuedouble : 5000, &e);
    } else if (!strcmp(op, "state")) res = bridge_state(&e);
    else if (!strcmp(op, "info")) res = bridge_info();
    else if (!strcmp(op, "scpi")) res = bridge_scpi(cJSON_GetObjectItemCaseSensitive(msg, "cmd"), &e);
    else if (!strcmp(op, "leads")) res = msg->child ? bridge_set_leads(msg, &e) : bridge_leads();
    else {
        snprintf(e.code, sizeof e.code, "bad_request");
        snprintf(e.message, sizeof e.message, "unknown op: %s", op);
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "id", id ? id : cJSON_CreateNull());
    cJSON_AddBoolToObject(reply, "ok", res != NULL);
    cJSON *body = res ? res : bridge_error_body(&e);
    while (body->child) {
        cJSON *it = cJSON_DetachItemViaPointer(body, body->child);
        cJSON_AddItemToObject(reply, it->string, it);
    }
    cJSON_Delete(body);
    cJSON_Delete(opj);
    cJSON_Delete(msg);
    ws_send_json(fd, reply);
}

// A new WebSocket client: track it and queue its hello message.
static void ws_opened(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    ws_add(fd);
    xQueueSend(s_hello, &fd, 0);
}

// ESP-IDF 5.5.2 added a callback before the upgrade (a cross-origin page never gets a socket), and
// later 5.5 builds one after it, which then replaces the handler call for the handshake. Without
// them the handler is called once with HTTP_GET right after the upgrade (see ws_handler).
#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (origin_ok(req)) return ESP_OK;
    forbidden(req);
    return ESP_FAIL;
}
#endif

#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    ws_opened(req);
    return ESP_OK;
}
#endif

// Called for every data frame (PING, PONG and CLOSE are handled by the server), and on ESP-IDF
// without the post-handshake callback once more with HTTP_GET when the upgrade is done.
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
#ifndef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
        if (!origin_ok(req)) return ESP_FAIL;  // closes the connection
#endif
        ws_opened(req);
        return ESP_OK;
    }
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len > MAX_WS_MSG) return ESP_FAIL;  // closes the connection
    char *buf = calloc(1, f.len + 1);
    if (!buf) return ESP_FAIL;
    if (f.len) {
        f.payload = (uint8_t *)buf;
        if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
    }
    if (f.type != HTTPD_WS_TYPE_TEXT) {
        free(buf);
        return ESP_OK;
    }
    cJSON *msg = cJSON_Parse(buf);
    bool object = cJSON_IsObject(msg);
    cJSON *id = object ? cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(msg, "id"), true) : NULL;
    cJSON_Delete(msg);
    if (!object) {
        free(buf);
        ws_send_json(fd, cJSON_Parse("{\"ok\":false,\"error\":\"bad_request\",\"message\":\"messages must be JSON objects\"}"));
        return ESP_OK;
    }
    work_t w = {.kind = W_WS_OP, .fd = fd, .body = buf};
    if (xQueueSend(s_work, &w, 0) != pdTRUE) {
        free(buf);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "id", id ? id : cJSON_CreateNull());
        id = NULL;
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "busy");
        cJSON_AddStringToObject(r, "message", "too many requests in flight");
        ws_send_json(fd, r);
    }
    cJSON_Delete(id);
    return ESP_OK;
}

static void broadcast_task(void *arg)
{
    QueueHandle_t sub = meter_subscribe();
    for (;;) {
        int fd;
        while (xQueueReceive(s_hello, &fd, 0) == pdTRUE) ws_hello(fd);
        meter_event_t ev;
        if (!sub || xQueueReceive(sub, &ev, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        int fds[MAX_WS];
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        memcpy(fds, s_ws_fds, sizeof fds);
        xSemaphoreGive(s_ws_lock);
        bool any = false;
        for (int i = 0; i < MAX_WS; i++) any |= fds[i] != 0;
        if (!any) continue;
        cJSON *j = bridge_event_json(&ev);
        char *s = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        for (int i = 0; s && i < MAX_WS; i++)
            if (fds[i]) ws_send(fds[i], s);
        cJSON_free(s);
    }
}

static void worker_task(void *arg)
{
    for (;;) {
        work_t w;
        xQueueReceive(s_work, &w, portMAX_DELAY);
        if (w.kind == W_WS_OP) {
            ws_op(w.fd, w.body);
        } else {
            http_work(&w);
            httpd_req_async_handler_complete(w.req);
        }
        free(w.body);
    }
}

// ------------------------------------------------------------------ REST handlers
static esp_err_t index_get(httpd_req_t *req)
{
    if (net_setup_active()) return portal_page(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);  // TXT adds a NUL
}

static esp_err_t api_index(httpd_req_t *req)
{
    if (!origin_ok(req)) return forbidden(req);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", "metermind");
    cJSON_AddStringToObject(o, "version", bridge_version());
    cJSON *ep = cJSON_AddObjectToObject(o, "endpoints");
    cJSON_AddStringToObject(ep, "GET /api/info", "identity and connection status");
    cJSON_AddStringToObject(ep, "GET /api/state", "current settings");
    cJSON_AddStringToObject(ep, "GET /api/reading?fresh=1&timeout_ms=5000", "latest valid reading");
    cJSON_AddStringToObject(ep, "PUT /api/config", "{function, range, rate, beep}: set, verify, wait for settle");
    cJSON_AddStringToObject(ep, "POST /api/measure", "{function, range, rate, samples}: configure + sample + stats");
    cJSON_AddStringToObject(ep, "POST /api/scpi", "{cmd}: raw SCPI escape hatch");
    cJSON_AddStringToObject(ep, "GET /api/leads", "where the user says the red lead is");
    cJSON_AddStringToObject(ep, "PUT /api/leads", "{red: unknown|none|input|ma|10a}: set it (config and measure check it)");
    cJSON_AddStringToObject(ep, "GET /ws", "WebSocket: reading/state/status events, accepts {id, op, ...}");
    cJSON_AddStringToObject(ep, "POST /mcp", "MCP endpoint (Streamable HTTP, JSON responses)");
    cJSON_AddStringToObject(ep, "GET /api/wifi", "WiFi status (firmware only)");
    cJSON_AddStringToObject(ep, "GET /api/wifi/scan?refresh=1", "nearby networks (setup mode only)");
    cJSON_AddStringToObject(ep, "POST /api/wifi", "{ssid, password}: try a network, saved once it works (setup mode only)");
    cJSON_AddStringToObject(ep, "POST /api/wifi/done", "close the setup network (setup mode only)");
    return send_json(req, 200, o);
}

static esp_err_t api_info(httpd_req_t *req)
{
    if (!origin_ok(req)) return forbidden(req);
    return send_json(req, 200, bridge_info());
}

static esp_err_t api_state(httpd_req_t *req)
{
    if (!origin_ok(req)) return forbidden(req);
    berr_t e = {0};
    cJSON *s = bridge_state(&e);
    return s ? send_json(req, 200, s) : send_error(req, &e);
}

static esp_err_t api_reading(httpd_req_t *req)
{
    if (!origin_ok(req)) return forbidden(req);
    char q[128] = "", v[32];
    bool fresh = false;
    int timeout_ms = 5000;
    httpd_req_get_url_query_str(req, q, sizeof q);
    if (httpd_query_key_value(q, "fresh", v, sizeof v) == ESP_OK)
        fresh = !strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes");
    if (httpd_query_key_value(q, "timeout_ms", v, sizeof v) == ESP_OK) {
        char *end;
        long t = strtol(v, &end, 10);
        if (!*v || *end) return send_simple_error(req, "bad_request", "timeout_ms must be an integer");
        timeout_ms = (int)t;
    }
    if (!fresh) {
        berr_t e = {0};
        cJSON *r = bridge_reading(false, timeout_ms, &e);
        return r ? send_json(req, 200, r) : send_error(req, &e);
    }
    return go_async(req, W_READING, true, timeout_ms);
}

static esp_err_t api_config(httpd_req_t *req)
{
    return origin_ok(req) ? go_async(req, W_CONFIG, false, 0) : forbidden(req);
}

static esp_err_t api_measure(httpd_req_t *req)
{
    return origin_ok(req) ? go_async(req, W_MEASURE, false, 0) : forbidden(req);
}

static esp_err_t api_leads(httpd_req_t *req)
{
    if (!origin_ok(req)) return forbidden(req);
    if (req->method == HTTP_GET) return send_json(req, 200, bridge_leads());
    berr_t e = {0};
    char *body = NULL;
    if (!read_body(req, &body, &e)) return send_error(req, &e);
    cJSON *j = parse_body(body, &e);
    free(body);
    if (!j) return send_error(req, &e);
    cJSON *r = bridge_set_leads(j, &e);
    cJSON_Delete(j);
    return r ? send_json(req, 200, r) : send_error(req, &e);
}

static esp_err_t api_scpi(httpd_req_t *req)
{
    return origin_ok(req) ? go_async(req, W_SCPI, false, 0) : forbidden(req);
}

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    return origin_ok(req) ? go_async(req, W_MCP, false, 0) : forbidden(req);
}

static esp_err_t mcp_not_allowed(httpd_req_t *req)
{
    httpd_resp_set_status(req, status_line(405));
    httpd_resp_set_hdr(req, "Allow", "POST");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (net_setup_active()) return portal_redirect(req);
    return send_simple_error(req, "not_found", "no such endpoint; GET /api lists them");
}

static void on_close(httpd_handle_t hd, int fd)
{
    ws_remove(fd);
    close(fd);
}

void http_start(void)
{
    s_work = xQueueCreate(WORK_DEPTH, sizeof(work_t));
    s_hello = xQueueCreate(MAX_WS, sizeof(int));
    s_ws_lock = xSemaphoreCreateMutex();

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = CONFIG_BRIDGE_HTTP_PORT;
    c.max_uri_handlers = 24;
    c.max_open_sockets = 9;
    c.lru_purge_enable = true;
    c.close_fn = on_close;
    c.stack_size = 6144;
    if (httpd_start(&s_server, &c) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api", .method = HTTP_GET, .handler = api_index},
        {.uri = "/api/info", .method = HTTP_GET, .handler = api_info},
        {.uri = "/api/state", .method = HTTP_GET, .handler = api_state},
        {.uri = "/api/reading", .method = HTTP_GET, .handler = api_reading},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = api_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = api_config},
        {.uri = "/api/measure", .method = HTTP_POST, .handler = api_measure},
        {.uri = "/api/scpi", .method = HTTP_POST, .handler = api_scpi},
        {.uri = "/api/leads", .method = HTTP_GET, .handler = api_leads},
        {.uri = "/api/leads", .method = HTTP_PUT, .handler = api_leads},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
         .ws_pre_handshake_cb = ws_pre_handshake,
#endif
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
         .ws_post_handshake_cb = ws_post_handshake,
#endif
        },
        {.uri = "/mcp", .method = HTTP_POST, .handler = mcp_post_handler},
        {.uri = "/mcp", .method = HTTP_GET, .handler = mcp_not_allowed},
        {.uri = "/mcp", .method = HTTP_DELETE, .handler = mcp_not_allowed},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_server, &uris[i]);
    portal_register(s_server);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);

    for (int i = 0; i < NWORKERS; i++) xTaskCreate(worker_task, "http_work", 6144, NULL, 5, NULL);
    xTaskCreate(broadcast_task, "ws_push", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "HTTP, WebSocket and MCP on port %d", CONFIG_BRIDGE_HTTP_PORT);
}
