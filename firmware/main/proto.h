// XDM1041 protocol facts: function and range tables, reply parsing. No ESP-IDF dependencies,
// so tests/test_proto.c can build it on the host. Mirrors the tables in bridge/meter.py; see
// PROTOCOL.md for where every value comes from.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROTO_NFUNCS 11
#define PROTO_NRATES 3
#define PROTO_MAX_RANGES 7

typedef struct {
    const char *name;        // API name: "dcv"
    const char *conf_cmd;    // "CONF:VOLT:DC"
    const char *func_reply;  // FUNC? reply without quotes: "VOLT"
    const char *unit;        // "V"
    int nranges;             // 0 = no selectable range
    double ranges[PROTO_MAX_RANGES];  // full scale in SI units, RANGE <n> index order
} proto_func_t;

extern const proto_func_t PROTO_FUNCS[PROTO_NFUNCS];
extern const char *const PROTO_RATE_NAMES[PROTO_NRATES];  // "slow", "medium", "fast"
extern const char *const PROTO_RATE_CODES[PROTO_NRATES];  // "S", "M", "F"
extern const double PROTO_UPDATE_INTERVAL_S[PROTO_NRATES];

int proto_func_index(const char *name);          // -1 if unknown
int proto_func_by_reply(const char *quoted);     // FUNC? reply with quotes; -1 if not recognised
int proto_rate_index(const char *name);          // -1 if unknown
int proto_rate_by_code(const char *code);        // "S" -> 0; -1 if unknown

// Smallest range that can show `value`. Returns the RANGE index (1-based) and the full scale,
// or 0 when value exceeds the top range.
int proto_select_range(int func, double value, double *full_scale);

// "500 mV" -> 0.5, "5uF" -> 5e-6, "50 MΩ" -> 5e7. False for "Hz", "", "KITS90".
bool proto_parse_range(const char *label, double *out);

// RANGE? replies carry a unit ("5 V", "Hz") or are empty. A bare number is a stray reply.
bool proto_valid_range_label(const char *label);

// MEAS? always answers in scientific notation ("4.58E-05", "1E+9").
bool proto_valid_measurement(const char *raw);

// MEAS:SHOW? arrives in GB2312 (Ω is A6 B8). Converts the few non-ASCII characters the meter
// uses to UTF-8; any other high byte becomes U+FFFD. Always NUL-terminates.
void proto_gb2312_to_utf8(const char *in, char *out, size_t out_len);

// True if s is valid UTF-8.
bool proto_utf8_valid(const char *s);
