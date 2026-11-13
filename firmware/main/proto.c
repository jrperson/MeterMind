#include "proto.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const proto_func_t PROTO_FUNCS[PROTO_NFUNCS] = {
    {"dcv", "CONF:VOLT:DC", "VOLT", "V", 6, {0.05, 0.5, 5, 50, 500, 1000}},
    {"acv", "CONF:VOLT:AC", "VOLT AC", "V", 5, {0.5, 5, 50, 500, 750}},
    {"dci", "CONF:CURR:DC", "CURR", "A", 6, {500e-6, 5e-3, 50e-3, 500e-3, 5, 10}},
    {"aci", "CONF:CURR:AC", "CURR AC", "A", 6, {500e-6, 5e-3, 50e-3, 500e-3, 5, 10}},
    {"res", "CONF:RES", "RES", "ohm", 6, {500, 5e3, 50e3, 500e3, 5e6, 50e6}},
    {"cap", "CONF:CAP", "CAP", "F", 7, {50e-9, 500e-9, 5e-6, 50e-6, 500e-6, 5e-3, 50e-3}},
    {"freq", "CONF:FREQ", "FREQ", "Hz", 0, {0}},
    {"period", "CONF:PER", "PER", "s", 0, {0}},
    {"diode", "CONF:DIOD", "DIOD", "V", 0, {0}},
    {"continuity", "CONF:CONT", "CONT", "ohm", 0, {0}},
    {"temp", "CONF:TEMP", "TEMP", "C", 0, {0}},
};

const char *const PROTO_RATE_NAMES[PROTO_NRATES] = {"slow", "medium", "fast"};
const char *const PROTO_RATE_CODES[PROTO_NRATES] = {"S", "M", "F"};
// Measured interval between new readings at each rate (PROTOCOL.md).
const double PROTO_UPDATE_INTERVAL_S[PROTO_NRATES] = {1.0, 0.67, 0.33};

int proto_func_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < PROTO_NFUNCS; i++)
        if (strcmp(PROTO_FUNCS[i].name, name) == 0) return i;
    return -1;
}

int proto_func_by_reply(const char *quoted)
{
    if (!quoted || quoted[0] != '"') return -1;  // FUNC? is always a quoted string
    size_t n = strlen(quoted);
    if (n < 2 || quoted[n - 1] != '"') return -1;
    for (int i = 0; i < PROTO_NFUNCS; i++) {
        const char *r = PROTO_FUNCS[i].func_reply;
        if (strlen(r) == n - 2 && strncmp(quoted + 1, r, n - 2) == 0) return i;
    }
    return -1;
}

int proto_rate_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < PROTO_NRATES; i++)
        if (strcmp(PROTO_RATE_NAMES[i], name) == 0) return i;
    return -1;
}

int proto_rate_by_code(const char *code)
{
    if (!code) return -1;
    for (int i = 0; i < PROTO_NRATES; i++)
        if (strcmp(PROTO_RATE_CODES[i], code) == 0) return i;
    return -1;
}

int proto_select_range(int func, double value, double *full_scale)
{
    const proto_func_t *f = &PROTO_FUNCS[func];
    for (int i = 0; i < f->nranges; i++) {
        if (f->ranges[i] >= value * (1 - 1e-9)) {
            if (full_scale) *full_scale = f->ranges[i];
            return i + 1;
        }
    }
    return 0;
}

bool proto_parse_range(const char *label, double *out)
{
    // Python: ^\s*([0-9]*\.?[0-9]+)\s*([numµkKMG]?)
    if (!label) return false;
    const char *p = label;
    while (isspace((unsigned char)*p)) p++;
    const char *start = p;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.' && isdigit((unsigned char)p[1])) {  // "5." parses as 5, like the regex
        p++;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (p == start) return false;
    char num[24];
    size_t n = (size_t)(p - start);
    if (n >= sizeof num) return false;
    memcpy(num, start, n);
    num[n] = 0;
    double v = strtod(num, NULL);
    while (isspace((unsigned char)*p)) p++;
    double mult = 1.0;
    switch (*p) {
    case 'n': mult = 1e-9; break;
    case 'u': mult = 1e-6; break;
    case 'm': mult = 1e-3; break;
    case 'k': case 'K': mult = 1e3; break;
    case 'M': mult = 1e6; break;
    case 'G': mult = 1e9; break;
    case '\xC2': if ((unsigned char)p[1] == 0xB5) mult = 1e-6; break;  // µ in UTF-8
    default: break;
    }
    if (out) *out = v * mult;
    return true;
}

static bool number_only(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!(isdigit((unsigned char)*s) || *s == '-' || *s == '+' || *s == '.' || *s == 'E' || *s == 'e'))
            return false;
    return true;
}

bool proto_valid_range_label(const char *label)
{
    return label && !number_only(label);
}

bool proto_valid_measurement(const char *raw)
{
    return raw && strchr(raw, 'E') && number_only(raw);
}

static size_t put(char *out, size_t pos, size_t len, const char *s)
{
    size_t n = strlen(s);
    if (pos + n >= len) return pos;
    memcpy(out + pos, s, n);
    return pos + n;
}

void proto_gb2312_to_utf8(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out_len) return;
    for (const unsigned char *p = (const unsigned char *)in; *p;) {
        if (*p < 0x80) {
            if (o + 1 < out_len) out[o++] = (char)*p;
            p++;
            continue;
        }
        const char *u = "\xEF\xBF\xBD";  // U+FFFD
        if (p[1]) {
            unsigned code = (unsigned)p[0] << 8 | p[1];
            switch (code) {
            case 0xA6B8: u = "\xCE\xA9"; break;  // Ω
            case 0xA6CC: u = "\xCE\xBC"; break;  // μ
            case 0xA1E3: u = "\xC2\xB0"; break;  // °
            case 0xA1E6: u = "\xE2\x84\x83"; break;  // ℃
            default: break;
            }
            p += 2;
        } else {
            p++;
        }
        o = put(out, o, out_len, u);
    }
    out[o] = 0;
}

bool proto_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if (*p < 0x80) n = 0;
        else if ((*p & 0xE0) == 0xC0 && *p >= 0xC2) n = 1;
        else if ((*p & 0xF0) == 0xE0) n = 2;
        else if ((*p & 0xF8) == 0xF0 && *p <= 0xF4) n = 3;
        else return false;
        p++;
        for (int i = 0; i < n; i++, p++)
            if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}
