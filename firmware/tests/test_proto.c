// Host test for main/proto.c. Expected values come from bridge/meter.py (run_tests.sh checks the
// Python side prints the same table). Build and run: firmware/tests/run_tests.sh
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/proto.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void range(const char *label, int ok, double want)
{
    double v = 0;
    bool got = proto_parse_range(label, &v);
    CHECK(got == ok && (!ok || fabs(v - want) <= fabs(want) * 1e-12), "parse_range(%s) -> %d %g", label, got, v);
    printf("range %s|%s\n", label, got ? "" : "None");
    if (got) printf("value %.12g\n", v);
}

int main(void)
{
    range("500 mV", 1, 0.5);
    range("5uF", 1, 5e-6);
    range("50 MΩ", 1, 5e7);
    range("5 KΩ", 1, 5e3);
    range("500 Ω", 1, 500);
    range("10 A", 1, 10);
    range("500 uA", 1, 500e-6);
    range("5 mF", 1, 5e-3);
    range("Hz", 0, 0);
    range("", 0, 0);
    range("KITS90", 0, 0);
    range(".5 V", 1, 0.5);
    range("5. V", 1, 5);
    range("1000 V", 1, 1000);

    double fs = 0;
    int dcv = proto_func_index("dcv"), res = proto_func_index("res"), cap = proto_func_index("cap");
    CHECK(proto_select_range(dcv, 30, &fs) == 4 && fs == 50, "dcv 30");
    CHECK(proto_select_range(dcv, 5, &fs) == 3 && fs == 5, "dcv 5");
    CHECK(proto_select_range(dcv, 0.05, &fs) == 1, "dcv 0.05");
    CHECK(proto_select_range(dcv, 1001, &fs) == 0, "dcv 1001");
    CHECK(proto_select_range(res, 5000, &fs) == 2 && fs == 5000, "res 5000");
    CHECK(proto_select_range(cap, 1e-6, &fs) == 3 && fs == 5e-6, "cap 1u");

    CHECK(proto_valid_measurement("4.582938E-05"), "meas sci");
    CHECK(proto_valid_measurement("1E+9"), "meas overload");
    CHECK(!proto_valid_measurement("1"), "meas stray AUTO? reply");
    CHECK(!proto_valid_measurement("\"VOLT\""), "meas func reply");
    CHECK(!proto_valid_measurement(NULL), "meas null");
    CHECK(proto_valid_range_label("5 V"), "label 5 V");
    CHECK(proto_valid_range_label(""), "label empty");
    CHECK(!proto_valid_range_label("1"), "label stray");
    CHECK(!proto_valid_range_label("4.5E-05"), "label meas");

    CHECK(proto_func_by_reply("\"VOLT AC\"") == proto_func_index("acv"), "func acv");
    CHECK(proto_func_by_reply("\"VOLT\"") == dcv, "func dcv");
    CHECK(proto_func_by_reply("1") == -1, "func stray");
    CHECK(proto_func_by_reply("VOLT") == -1, "func unquoted");
    CHECK(proto_rate_by_code("M") == 1, "rate M");

    char out[64];
    proto_gb2312_to_utf8("1.002 K\xA6\xB8", out, sizeof out);
    CHECK(strcmp(out, "1.002 K\xCE\xA9") == 0, "gb2312 ohm: %s", out);
    proto_gb2312_to_utf8("overload", out, sizeof out);
    CHECK(strcmp(out, "overload") == 0, "gb2312 ascii");
    CHECK(proto_utf8_valid("5 K\xCE\xA9"), "utf8 ok");
    CHECK(!proto_utf8_valid("5 K\xA6\xB8"), "utf8 gb2312 bytes");

    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
