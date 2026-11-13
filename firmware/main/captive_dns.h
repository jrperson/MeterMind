// Captive-portal DNS for the setup network: every A query is answered with the access point's
// own address, so a phone's connectivity check lands on the setup page.
#pragma once

#include <stdint.h>

void captive_dns_start(uint32_t ip);  // ip in network byte order (esp_ip4_addr_t.addr)
void captive_dns_stop(void);
