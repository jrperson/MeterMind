// WiFi, mDNS and SNTP. The station joins the network saved in NVS. Setup mode opens an access
// point ("MeterMind-XXXX") with a captive portal (portal.c) for choosing that network; it starts
// by itself when no network is saved, and when BOOT is held for 3 s.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { NET_IDLE, NET_CONNECTING, NET_UP } net_state_t;  // the station link
typedef enum { NET_TRY_NONE, NET_TRY_RUNNING, NET_TRY_OK, NET_TRY_FAILED } net_try_t;

typedef struct {
    net_state_t state;
    bool configured;         // a network is saved (or built in with CONFIG_BRIDGE_WIFI_SSID)
    bool setup;              // the setup access point is up
    char ssid[33];           // the station's network: the saved one, or the one being tried
    char ip[16];             // station address while NET_UP
    int rssi;                // dBm while NET_UP
    int retries, last_reason;
    char ap_ssid[33];
    int ap_clients;
    bool scanning;
    net_try_t attempt;       // the last network tried from the setup page
    char attempt_ssid[33];
    char attempt_msg[80];
    int close_in_s;          // seconds until the setup network closes, or -1
} net_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} net_network_t;

void net_start(void);
net_state_t net_get_state(void);
bool net_setup_active(void);
void net_get_status(net_status_t *out);
const char *net_ap_ip(void);  // "192.168.4.1"

void net_setup_start(void);
void net_setup_stop(void);    // refused (stays up) while no network is saved
void net_setup_toggle(void);  // the BOOT button

// Setup mode only: join the network, and save it once an address is assigned. False when not in
// setup mode or the fields are too long. Progress shows in net_get_status().attempt.
bool net_try_credentials(const char *ssid, const char *password);
// Saves the network at once and joins it (leaving setup mode): the serial console's `wifi`.
bool net_set_credentials(const char *ssid, const char *password);
void net_clear_credentials(void);

void net_scan(void);  // starts a scan (setup mode); the results arrive in net_networks()
int net_networks(net_network_t *out, int max);  // strongest first

void net_describe(char *buf, size_t len);  // one line for the console's `status`
