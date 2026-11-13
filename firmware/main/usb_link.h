// USB host link to the meter's CH340 (the meter's USB-B port is a UART behind a CH340).
// Only the meter task calls these, except link_init.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_init(void);                       // installs the USB host and CDC-ACM drivers
bool link_open(uint32_t wait_ms);           // waits up to wait_ms for the CH340 to enumerate
bool link_is_open(void);                    // false once the device has gone (closes the handle)
void link_close(void);
bool link_write_line(const char *cmd);      // appends "\n"
// Reads one line (without "\r\n") within timeout_ms. Returns its length, or -1 on timeout.
int link_read_line(char *buf, size_t len, uint32_t timeout_ms);
void link_reset_input(void);                // discards anything already received
void link_drain(uint32_t quiet_ms);         // discards input until the line is quiet for quiet_ms
