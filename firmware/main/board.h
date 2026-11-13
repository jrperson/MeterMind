// MeterMind board v0.1: meter VBUS switch (U3 TPS2051C), status LED (D2 SK6812), BOOT button.
// Pin map of the MeterMind v0.1 board.
#pragma once

#include <stdbool.h>

#define PIN_BOOT 0           // SW2, also the WiFi setup button (hold 3 s)
#define PIN_METER_PWR_EN 4   // U3 EN, active high, pulled low (VBUS off until we enable it)
#define PIN_METER_FLT_N 5    // U3 fault, open drain with R8 pull-up, active low
#define PIN_LED 48           // D2 SK6812 data

void board_init(void);            // GPIO setup; switches the meter's VBUS on
void board_start_ui(void);        // LED and BOOT-button task (after meter_start and net_start)
void board_meter_vbus(bool on);
bool board_meter_vbus_fault(void);
