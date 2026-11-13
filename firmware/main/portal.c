#include "portal.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "http.h"
#include "net.h"
#include "sdkconfig.h"

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

#define MAX_NETS 20

esp_err_t portal_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);  // TXT adds a NUL
}

// Phones probe a known URL (generate_204, hotspot-detect.html, connecttest.txt); the DNS sends
// it here and the redirect opens the setup page in their sign-in window.
esp_err_t portal_redirect(httpd_req_t *req)
{
    char loc[32];
    snprintf(loc, sizeof loc, "http://%s/", net_ap_ip());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "MeterMind setup", HTTPD_RESP_USE_STRLEN);  // iOS wants a body
}

static const char *state_name(net_state_t s)
{
    return s == NET_UP ? "up" : s == NET_CONNECTING ? "connecting" : "idle";
}

static const char *try_name(net_try_t t)
{
    return t == NET_TRY_RUNNING ? "trying" : t == NET_TRY_OK ? "ok" : "failed";
}

static void add_str_or_null(cJSON *o, const char *k, const char *v)
{
    if (v && *v) cJSON_AddStringToObject(o, k, v);
    else cJSON_AddNullToObject(o, k);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    if (!http_origin_ok(req)) return http_send_error(req, "forbidden", "cross-origin request refused");
    net_status_t s;
    net_get_status(&s);
    char host[48];
    snprintf(host, sizeof host, "%s.local", CONFIG_BRIDGE_HOSTNAME);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "setup", s.setup);
    cJSON_AddBoolToObject(o, "configured", s.configured);
    cJSON_AddStringToObject(o, "state", state_name(s.state));
    add_str_or_null(o, "ssid", s.ssid);
    add_str_or_null(o, "ip", s.state == NET_UP ? s.ip : NULL);
    if (s.state == NET_UP) cJSON_AddNumberToObject(o, "rssi", s.rssi);
    else cJSON_AddNullToObject(o, "rssi");
    cJSON_AddStringToObject(o, "hostname", host);
    cJSON_AddStringToObject(o, "ap_ssid", s.ap_ssid);
    cJSON_AddStringToObject(o, "ap_ip", net_ap_ip());
    if (s.close_in_s >= 0) cJSON_AddNumberToObject(o, "close_in_s", s.close_in_s);
    else cJSON_AddNullToObject(o, "close_in_s");
    if (s.attempt == NET_TRY_NONE) {
        cJSON_AddNullToObject(o, "attempt");
    } else {
        cJSON *a = cJSON_AddObjectToObject(o, "attempt");
        cJSON_AddStringToObject(a, "ssid", s.attempt_ssid);
        cJSON_AddStringToObject(a, "state", try_name(s.attempt));
        add_str_or_null(a, "message", s.attempt_msg);
    }
    return http_send_json(req, 200, o);
}

static esp_err_t setup_required(httpd_req_t *req)
{
    return http_send_error(req, "setup_required", "only in setup mode: hold BOOT for 3 s to open the setup network");
}

static esp_err_t wifi_scan(httpd_req_t *req)
{
    if (!http_origin_ok(req)) return http_send_error(req, "forbidden", "cross-origin request refused");
    if (!net_setup_active()) return setup_required(req);
    char q[32] = "", v[8];
    httpd_req_get_url_query_str(req, q, sizeof q);
    bool refresh = httpd_query_key_value(q, "refresh", v, sizeof v) == ESP_OK && strcmp(v, "0");
    if (refresh) net_scan();
    net_status_t s;
    net_get_status(&s);
    static net_network_t nets[MAX_NETS];  // handlers run on the one server task
    int n = net_networks(nets, MAX_NETS);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "scanning", s.scanning || refresh);
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(e, "secure", nets[i].secure);
        cJSON_AddItemToArray(arr, e);
    }
    return http_send_json(req, 200, o);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!http_origin_ok(req)) return http_send_error(req, "forbidden", "cross-origin request refused");
    if (!net_setup_active()) return setup_required(req);
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof body) return http_send_error(req, "bad_request", "send {\"ssid\", \"password\"}");
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return http_send_error(req, "bad_request", "could not read the request body");
        got += r;
    }
    body[len] = 0;
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof body);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(j, "password");
    esp_err_t r;
    if (!cJSON_IsString(ssid) || (pass && !cJSON_IsNull(pass) && !cJSON_IsString(pass))) {
        r = http_send_error(req, "bad_request", "ssid must be a string, password a string or absent");
    } else {
        const char *pw = cJSON_IsString(pass) ? pass->valuestring : "";
        size_t pl = strlen(pw);
        if (pl && (pl < 8 || pl > 63)) {
            r = http_send_error(req, "bad_request", "a WPA password has 8 to 63 characters");
        } else if (!net_try_credentials(ssid->valuestring, pw)) {
            r = http_send_error(req, "bad_request", "the network name must have 1 to 32 bytes");
        } else {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", true);
            cJSON_AddStringToObject(o, "message", "trying; poll GET /api/wifi");
            r = http_send_json(req, 202, o);
        }
    }
    if (cJSON_IsString(pass)) memset(pass->valuestring, 0, strlen(pass->valuestring));
    cJSON_Delete(j);
    return r;
}

static esp_err_t wifi_done(httpd_req_t *req)
{
    if (!http_origin_ok(req)) return http_send_error(req, "forbidden", "cross-origin request refused");
    if (!net_setup_active()) return setup_required(req);
    net_status_t s;
    net_get_status(&s);
    if (!s.configured) return http_send_error(req, "bad_request", "no network saved yet");
    net_setup_stop();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return http_send_json(req, 200, o);
}

void portal_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = portal_page},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan},
        {.uri = "/api/wifi/done", .method = HTTP_POST, .handler = wifi_done},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}
