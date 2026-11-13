// WiFi station plus the setup access point, mDNS and SNTP. Every WiFi state change runs on the
// default event loop task: the console, the BOOT button, the setup page and the timers post
// NET_EVENT requests instead of calling esp_wifi themselves, so no two of them race.
#include "net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "captive_dns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "net";
#define NVS_NS "net"
#define MAX_SCAN 20
#define TRY_TIMEOUT_S 25        // a network tried from the setup page must give an address by then
#define CLOSE_AFTER_OK_S 60     // the setup network stays up this long after a success
#define SETUP_IDLE_S (CONFIG_BRIDGE_SETUP_TIMEOUT_MIN * 60)

ESP_EVENT_DEFINE_BASE(NET_EVENT);
enum { EV_SETUP_START, EV_SETUP_STOP, EV_SETUP_TOGGLE, EV_TRY, EV_TRY_TIMEOUT, EV_SET, EV_SCAN };

typedef struct {
    char ssid[33];
    char pass[64];
} creds_t;

static esp_netif_t *s_sta, *s_ap;
static esp_timer_handle_t s_retry_timer, s_try_timer, s_setup_timer;
static SemaphoreHandle_t s_lock;  // guards s_st and s_scan for readers on other tasks
static net_status_t s_st = {.close_in_s = -1};
static net_network_t s_scan[MAX_SCAN];
static int s_nscan;
static char s_ap_ip[16] = "192.168.4.1";
static char s_portal_uri[32];   // DHCP option 114 keeps the pointer
static int s_retries;
static bool s_expect_leave;     // our own esp_wifi_disconnect(): its ASSOC_LEAVE is not a failure
static bool s_closing;          // the setup network closes when s_setup_timer fires, whatever the clients do
static int64_t s_close_at_us;
static wifi_config_t s_try_cfg;
static int s_try_attempts;
static bool s_scan_dropped;     // a scan stopped for a try: keep the previous list

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// ------------------------------------------------------------------ credentials
static bool load_credentials(wifi_config_t *wc)
{
    memset(wc, 0, sizeof *wc);
    nvs_handle_t h;
    size_t n;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        n = sizeof wc->sta.ssid;
        esp_err_t a = nvs_get_str(h, "ssid", (char *)wc->sta.ssid, &n);
        n = sizeof wc->sta.password;
        if (nvs_get_str(h, "pass", (char *)wc->sta.password, &n) != ESP_OK) wc->sta.password[0] = 0;
        nvs_close(h);
        if (a == ESP_OK && wc->sta.ssid[0]) return true;
    }
    if (CONFIG_BRIDGE_WIFI_SSID[0]) {
        snprintf((char *)wc->sta.ssid, sizeof wc->sta.ssid, "%s", CONFIG_BRIDGE_WIFI_SSID);
        snprintf((char *)wc->sta.password, sizeof wc->sta.password, "%s", CONFIG_BRIDGE_WIFI_PASSWORD);
        return true;
    }
    return false;
}

static bool save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool creds_fit(const char *ssid, const char *password)
{
    return ssid && *ssid && strlen(ssid) <= 32 && (!password || strlen(password) <= 63);
}

static bool post(int32_t id, const void *data, size_t len)
{
    return esp_event_post(NET_EVENT, id, data, len, pdMS_TO_TICKS(100)) == ESP_OK;
}

// ------------------------------------------------------------------ station
static void sta_connect(const wifi_config_t *cfg)
{
    wifi_config_t wc = *cfg;
    wc.sta.threshold.authmode = wc.sta.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    LOCK();
    s_st.state = NET_CONNECTING;
    snprintf(s_st.ssid, sizeof s_st.ssid, "%s", (char *)wc.sta.ssid);
    s_st.ip[0] = 0;
    s_st.retries = 0;
    UNLOCK();
    s_retries = 0;
    ESP_LOGI(TAG, "connecting to \"%s\"", s_st.ssid);
    esp_wifi_connect();
}

static void sta_stop(void)
{
    esp_timer_stop(s_retry_timer);
    if (s_st.state != NET_IDLE) {
        s_expect_leave = true;
        esp_wifi_disconnect();
    }
    LOCK();
    s_st.state = NET_IDLE;
    s_st.ip[0] = 0;
    UNLOCK();
}

static void retry_cb(void *arg)
{
    if (s_st.state == NET_CONNECTING) esp_wifi_connect();
}

static bool auth_failure(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_MIC_FAILURE || reason == WIFI_REASON_CONNECTION_FAIL;
}

static const char *reason_text(int reason)
{
    if (auth_failure(reason)) return "wrong password";
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found (out of range, or 5 GHz only)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "the network's security does not match (open network: leave the password empty)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "signal too weak";
    default: return NULL;
    }
}

// ------------------------------------------------------------------ setup mode
static void arm_setup_timer(int seconds, bool closing)
{
    esp_timer_stop(s_setup_timer);
    s_closing = closing;
    s_close_at_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    esp_timer_start_once(s_setup_timer, (uint64_t)seconds * 1000000);
}

static void disarm_setup_timer(void)
{
    esp_timer_stop(s_setup_timer);
    s_closing = false;
    s_close_at_us = 0;
}

// The idle timeout: a setup network nobody uses closes again, unless there is nothing to go back to.
static void maybe_arm_idle(void)
{
    if (s_st.setup && !s_closing && s_st.configured && s_st.ap_clients == 0 && s_st.attempt != NET_TRY_RUNNING)
        arm_setup_timer(SETUP_IDLE_S, false);
}

static void scan_start(void)
{
    if (s_st.scanning || s_st.state == NET_CONNECTING) return;  // the driver refuses a scan while connecting
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        LOCK();
        s_st.scanning = true;
        UNLOCK();
    }
}

static void scan_done(void)
{
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof *recs);
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    esp_wifi_clear_ap_list();
    if (s_scan_dropped) {
        s_scan_dropped = false;
        free(recs);
        return;
    }
    static net_network_t list[MAX_SCAN];  // 800 bytes: too much for the event task's stack
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        int j = 0;
        while (j < count && strcmp(list[j].ssid, ssid)) j++;
        if (j == count) {  // a new name (several access points can share one)
            snprintf(list[count].ssid, sizeof list[count].ssid, "%s", ssid);
            list[count].rssi = recs[i].rssi;
            list[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            count++;
        } else if (recs[i].rssi > list[j].rssi) {
            list[j].rssi = recs[i].rssi;
        }
    }
    free(recs);
    for (int i = 1; i < count; i++)  // strongest first
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            net_network_t t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    LOCK();
    memcpy(s_scan, list, sizeof list);
    s_nscan = count;
    s_st.scanning = false;
    UNLOCK();
    ESP_LOGI(TAG, "scan: %d networks", count);
}

static void setup_start(void)
{
    if (s_st.setup) return;
    sta_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", s_st.ap_ssid);
    ap.ap.ssid_len = strlen(s_st.ap_ssid);
    ap.ap.channel = 1;  // follows the station's channel once it joins a network
    ap.ap.max_connection = 4;
    if (strlen(CONFIG_BRIDGE_SETUP_PASSWORD) >= 8) {
        snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", CONFIG_BRIDGE_SETUP_PASSWORD);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    LOCK();
    s_st.setup = true;
    s_st.ap_clients = 0;
    s_st.attempt = NET_TRY_NONE;
    s_st.attempt_ssid[0] = s_st.attempt_msg[0] = 0;
    UNLOCK();
    disarm_setup_timer();
    maybe_arm_idle();
    scan_start();
    ESP_LOGW(TAG, "setup mode: join WiFi \"%s\" and open http://%s/", s_st.ap_ssid, s_ap_ip);
}

static void setup_stop(void)
{
    if (!s_st.setup) return;
    if (!s_st.configured) {
        ESP_LOGW(TAG, "no network saved yet: staying in setup mode");
        return;
    }
    disarm_setup_timer();
    esp_timer_stop(s_try_timer);
    if (s_st.attempt == NET_TRY_RUNNING) sta_stop();  // abandon a half-finished try
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    LOCK();
    s_st.setup = false;
    s_st.ap_clients = 0;
    if (s_st.attempt == NET_TRY_RUNNING) s_st.attempt = NET_TRY_NONE;
    UNLOCK();
    ESP_LOGI(TAG, "setup mode off");
    wifi_config_t wc;
    if (s_st.state == NET_IDLE && load_credentials(&wc)) sta_connect(&wc);
}

static void try_finish(bool ok, const char *msg)
{
    esp_timer_stop(s_try_timer);
    LOCK();
    s_st.attempt = ok ? NET_TRY_OK : NET_TRY_FAILED;
    snprintf(s_st.attempt_msg, sizeof s_st.attempt_msg, "%s", msg);
    UNLOCK();
    if (ok) {
        arm_setup_timer(CLOSE_AFTER_OK_S, true);
        return;
    }
    ESP_LOGW(TAG, "\"%s\": %s", s_st.attempt_ssid, msg);
    sta_stop();
    maybe_arm_idle();
}

static void try_start(const creds_t *c)
{
    if (!s_st.setup) return;
    sta_stop();
    memset(&s_try_cfg, 0, sizeof s_try_cfg);
    snprintf((char *)s_try_cfg.sta.ssid, sizeof s_try_cfg.sta.ssid, "%s", c->ssid);
    snprintf((char *)s_try_cfg.sta.password, sizeof s_try_cfg.sta.password, "%s", c->pass);
    s_try_attempts = 1;
    if (s_st.scanning && esp_wifi_scan_stop() == ESP_OK) s_scan_dropped = true;
    LOCK();
    s_st.attempt = NET_TRY_RUNNING;
    snprintf(s_st.attempt_ssid, sizeof s_st.attempt_ssid, "%s", c->ssid);
    s_st.attempt_msg[0] = 0;
    s_st.last_reason = 0;
    s_st.scanning = false;
    UNLOCK();
    disarm_setup_timer();
    esp_timer_start_once(s_try_timer, (uint64_t)TRY_TIMEOUT_S * 1000000);
    sta_connect(&s_try_cfg);
}

static void on_try_disconnect(int reason)
{
    const char *why = reason_text(reason);
    bool hopeless = (auth_failure(reason) && s_try_attempts >= 2) ||
                    (why && !auth_failure(reason) && s_try_attempts >= 3);
    if (hopeless) {
        try_finish(false, why);
        return;
    }
    s_try_attempts++;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, 1000000);
}

// ------------------------------------------------------------------ events
static void timer_post(void *arg) { post((int32_t)(intptr_t)arg, NULL, 0); }

static void on_net_event(int32_t id, void *data)
{
    switch (id) {
    case EV_SETUP_START: setup_start(); break;
    case EV_SETUP_STOP: setup_stop(); break;
    case EV_SETUP_TOGGLE:
        if (s_st.setup) setup_stop();
        else setup_start();
        break;
    case EV_TRY: try_start(data); break;
    case EV_TRY_TIMEOUT:
        if (s_st.attempt == NET_TRY_RUNNING) {
            const char *why = reason_text(s_st.last_reason);
            char msg[80];
            snprintf(msg, sizeof msg, "%s", why ? why : "no answer from the network (timed out)");
            try_finish(false, msg);
        }
        break;
    case EV_SET: {
        const creds_t *c = data;
        if (!save_credentials(c->ssid, c->pass)) {
            ESP_LOGE(TAG, "could not save the WiFi settings");
            break;
        }
        LOCK();
        s_st.configured = true;
        UNLOCK();
        wifi_config_t wc;
        load_credentials(&wc);
        if (s_st.setup) {
            sta_stop();
            setup_stop();  // connects with the saved network
        } else {
            sta_stop();
            sta_connect(&wc);
        }
        break;
    }
    case EV_SCAN: scan_start(); break;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == NET_EVENT) {
        on_net_event(id, data);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t wc;
        if (load_credentials(&wc)) {
            s_st.configured = true;
            sta_connect(&wc);
        } else {
            ESP_LOGW(TAG, "no WiFi network saved");
            setup_start();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // An IPv6 link-local address lets mDNS answer AAAA queries for metermind.local. Without
        // one, macOS waits 5 s for the AAAA answer on every lookup before using the IPv4 address.
        esp_netif_create_ip6_linklocal(s_sta);
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *g = data;
        ESP_LOGI(TAG, "IPv6 " IPV6STR, IPV62STR(g->ip6_info.ip));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        // DHCP option 114 (RFC 8910) points phones at the portal; DNS catches the rest.
        esp_netif_dhcps_stop(s_ap);
        esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri,
                               strlen(s_portal_uri));
        esp_netif_dhcps_start(s_ap);
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_ap, &ip);
        captive_dns_start(ip.ip.addr);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        LOCK();
        s_st.ap_clients++;
        UNLOCK();
        if (!s_closing) disarm_setup_timer();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        LOCK();
        if (s_st.ap_clients > 0) s_st.ap_clients--;
        UNLOCK();
        maybe_arm_idle();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool own = s_expect_leave && d->reason == WIFI_REASON_ASSOC_LEAVE;
        s_expect_leave = false;
        if (own || s_st.state == NET_IDLE) return;
        LOCK();
        s_st.last_reason = d->reason;
        s_st.ip[0] = 0;
        UNLOCK();
        if (s_st.attempt == NET_TRY_RUNNING) {
            on_try_disconnect(d->reason);
            return;
        }
        if (s_st.setup) {  // a network that worked dropped while the setup page is still up
            sta_stop();
            return;
        }
        int delay_s = s_retries < 5 ? 1 : (s_retries < 20 ? 5 : 30);  // back off, never give up
        s_retries++;
        LOCK();
        s_st.state = NET_CONNECTING;
        s_st.retries = s_retries;
        UNLOCK();
        ESP_LOGW(TAG, "WiFi disconnected (reason %d), retry %d in %d s", d->reason, s_retries, delay_s);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *g = data;
        s_expect_leave = false;
        s_retries = 0;
        LOCK();
        s_st.state = NET_UP;
        s_st.retries = 0;
        snprintf(s_st.ip, sizeof s_st.ip, IPSTR, IP2STR(&g->ip_info.ip));
        UNLOCK();
        ESP_LOGI(TAG, "on \"%s\" as %s: http://%s.local/  (MCP: /mcp)", s_st.ssid, s_st.ip, CONFIG_BRIDGE_HOSTNAME);
        if (s_st.attempt == NET_TRY_RUNNING) {
            if (save_credentials((char *)s_try_cfg.sta.ssid, (char *)s_try_cfg.sta.password)) {
                LOCK();
                s_st.configured = true;
                UNLOCK();
                try_finish(true, "connected; saved");
            } else {
                try_finish(false, "connected, but the settings could not be saved");
            }
        }
    }
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable");
        return;
    }
    mdns_hostname_set(CONFIG_BRIDGE_HOSTNAME);
    mdns_instance_name_set("MeterMind (XDM1041 multimeter)");
    mdns_txt_item_t txt[] = {
        {"path", "/api"}, {"mcp", "/mcp"}, {"ws", "/ws"}, {"version", bridge_version()},
    };
    size_t n = sizeof txt / sizeof txt[0];
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_BRIDGE_HTTP_PORT, txt, n);
    mdns_service_add(NULL, "_metermind", "_tcp", CONFIG_BRIDGE_HTTP_PORT, txt, n);
    if (CONFIG_BRIDGE_SCPI_PORT) mdns_service_add(NULL, "_scpi-raw", "_tcp", CONFIG_BRIDGE_SCPI_PORT, NULL, 0);
}

void net_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, CONFIG_BRIDGE_HOSTNAME);
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) snprintf(s_ap_ip, sizeof s_ap_ip, IPSTR, IP2STR(&ip.ip));
    snprintf(s_portal_uri, sizeof s_portal_uri, "http://%s/", s_ap_ip);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_st.ap_ssid, sizeof s_st.ap_ssid, "MeterMind-%02X%02X", mac[4], mac[5]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials are kept in our own NVS namespace
    esp_timer_create_args_t t = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_retry_timer));
    t = (esp_timer_create_args_t){.callback = timer_post, .arg = (void *)(intptr_t)EV_TRY_TIMEOUT, .name = "wifi_try"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_try_timer));
    t = (esp_timer_create_args_t){.callback = timer_post, .arg = (void *)(intptr_t)EV_SETUP_STOP, .name = "wifi_setup"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_setup_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // lowest latency; the board is mains powered via its charger
    start_mdns();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);  // only for the optional "ts" field on readings
}

// ------------------------------------------------------------------ API for other tasks
net_state_t net_get_state(void) { return s_st.state; }
bool net_setup_active(void) { return s_st.setup; }
const char *net_ap_ip(void) { return s_ap_ip; }

void net_get_status(net_status_t *out)
{
    LOCK();
    *out = s_st;
    int64_t left = s_close_at_us ? (s_close_at_us - esp_timer_get_time()) / 1000000 : -1;
    UNLOCK();
    out->close_in_s = s_st.setup && s_close_at_us ? (int)(left < 0 ? 0 : left) : -1;
    out->rssi = 0;
    wifi_ap_record_t ap;
    if (out->state == NET_UP && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}

int net_networks(net_network_t *out, int max)
{
    LOCK();
    int n = s_nscan < max ? s_nscan : max;
    memcpy(out, s_scan, n * sizeof *out);
    UNLOCK();
    return n;
}

void net_setup_start(void) { post(EV_SETUP_START, NULL, 0); }
void net_setup_stop(void) { post(EV_SETUP_STOP, NULL, 0); }
void net_setup_toggle(void) { post(EV_SETUP_TOGGLE, NULL, 0); }
void net_scan(void) { post(EV_SCAN, NULL, 0); }

bool net_try_credentials(const char *ssid, const char *password)
{
    if (!s_st.setup || !creds_fit(ssid, password)) return false;
    creds_t c = {0};
    snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
    snprintf(c.pass, sizeof c.pass, "%s", password ? password : "");
    // Shown as running at once, so a status poll right after the POST never sees the old result.
    LOCK();
    s_st.attempt = NET_TRY_RUNNING;
    snprintf(s_st.attempt_ssid, sizeof s_st.attempt_ssid, "%s", ssid);
    s_st.attempt_msg[0] = 0;
    UNLOCK();
    bool ok = post(EV_TRY, &c, sizeof c);
    memset(&c, 0, sizeof c);
    return ok;
}

bool net_set_credentials(const char *ssid, const char *password)
{
    if (!creds_fit(ssid, password)) return false;
    creds_t c = {0};
    snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
    snprintf(c.pass, sizeof c.pass, "%s", password ? password : "");
    bool ok = post(EV_SET, &c, sizeof c);
    memset(&c, 0, sizeof c);
    return ok;
}

void net_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void net_describe(char *buf, size_t len)
{
    net_status_t s;
    net_get_status(&s);
    int n = 0;
    if (s.setup)
        n = snprintf(buf, len, "setup network \"%s\" up (%d joined): http://%s/\n", s.ap_ssid, s.ap_clients, s_ap_ip);
    if (n < 0 || (size_t)n >= len) return;
    buf += n;
    len -= n;
    switch (s.state) {
    case NET_IDLE:
        snprintf(buf, len, "WiFi: %s", s.configured ? (s.setup ? "paused for setup" : "idle") : "no network saved");
        break;
    case NET_CONNECTING:
        snprintf(buf, len, "WiFi: connecting to \"%s\" (attempt %d, last reason %d)", s.ssid, s.retries + 1,
                 s.last_reason);
        break;
    default:
        snprintf(buf, len, "WiFi: \"%s\" %s RSSI %d dBm, http://%s.local/", s.ssid, s.ip, s.rssi,
                 CONFIG_BRIDGE_HOSTNAME);
    }
}
