#include "mcp.h"

#include <string.h>

#include "bridge.h"

static const char *const SUPPORTED_VERSIONS[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

static const char INSTRUCTIONS[] =
    "Digcreat XDM1041 bench multimeter (5.5 digit) on the local network. Use `measure` for "
    "almost everything: it configures the meter, waits until readings are valid, and returns "
    "stats. Values are SI units (V, A, ohm, F, Hz, s). New readings arrive about 1/s at rate "
    "slow, 1.5/s medium, 3/s fast; a function or range change takes about 3 s to settle. "
    "The bridge cannot see where the test leads are plugged in. The user tells it which jack the "
    "red lead is in (get_state: state.leads is unknown, none, input, ma or 10a), and the bridge "
    "refuses functions and ranges that do not suit that jack with the error leads_mismatch. When "
    "that happens, ask the user to move the lead and update the setting on the dashboard; do not "
    "try to work around it. Current (dci/aci) also needs the meter in series with the load: never "
    "select a current function to measure across a voltage source.";

#define FUNC_ENUM "[\"dcv\",\"acv\",\"dci\",\"aci\",\"res\",\"cap\",\"freq\",\"period\",\"diode\",\"continuity\",\"temp\"]"
#define RATE_ENUM "[\"slow\",\"medium\",\"fast\"]"
#define FUNC_DESC                                                                                   \
    "\"Measurement function: dcv/acv (volts), dci/aci (amps), res (ohms), cap (farads), freq (Hz), " \
    "period (s), diode (V), continuity (ohms), temp (degrees)\""
#define RANGE_SCHEMA                                                                                     \
    "{\"description\":\"\\\"auto\\\" (default when a function is given), or full-scale range in SI units, " \
    "e.g. 0.5 for the 500 mV range or 5000 for 5 kohm. Rounds up to the next available range. Only for "  \
    "dcv, acv, dci, aci, res, cap.\",\"anyOf\":[{\"type\":\"string\",\"enum\":[\"auto\"]},"               \
    "{\"type\":\"number\",\"exclusiveMinimum\":0}]}"

// Same tool list as bridge/mcp.py TOOLS.
static const char TOOLS_JSON[] =
    "["
    "{\"name\":\"measure\",\"title\":\"Measure\","
    "\"description\":\"Take a measurement. Configures function/range/rate if needed, waits for valid readings, "
    "collects `samples` fresh readings and returns them with mean/min/max/stdev. `overload` true means out of "
    "range or open input.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"function\":{\"type\":\"string\",\"enum\":" FUNC_ENUM ",\"description\":" FUNC_DESC "},"
    "\"range\":" RANGE_SCHEMA ","
    "\"rate\":{\"type\":\"string\",\"enum\":" RATE_ENUM ",\"description\":\"Reading rate; slow is the most accurate\"},"
    "\"samples\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"default\":1}},"
    "\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true}},"

    "{\"name\":\"configure\",\"title\":\"Configure meter\","
    "\"description\":\"Change meter settings without taking a measurement. Returns the verified state and the "
    "first valid reading.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"function\":{\"type\":\"string\",\"enum\":" FUNC_ENUM ",\"description\":" FUNC_DESC "},"
    "\"range\":" RANGE_SCHEMA ","
    "\"rate\":{\"type\":\"string\",\"enum\":" RATE_ENUM "},"
    "\"beep\":{\"type\":\"boolean\",\"description\":\"Front-panel beeper on/off\"}},"
    "\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true}},"

    "{\"name\":\"get_reading\",\"title\":\"Latest reading\","
    "\"description\":\"Return the latest valid reading without changing anything. Set fresh=true to wait for a "
    "reading taken after this call.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"fresh\":{\"type\":\"boolean\",\"default\":false}},"
    "\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":true}},"

    "{\"name\":\"get_state\",\"title\":\"Meter state\","
    "\"description\":\"Return meter identity and current settings (function, range, autorange, rate) "
    "and where the user says the red lead is (leads).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":true}},"

    "{\"name\":\"scpi\",\"title\":\"Raw SCPI command\","
    "\"description\":\"Send a raw SCPI command for features not covered by other tools. Queries (containing '?') "
    "return the reply, or null if the meter does not support the command. Prefer `measure` and `configure`.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"e.g. "
    "CONT:THRE?\"}},\"required\":[\"command\"],\"additionalProperties\":false},"
    "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false}}"
    "]";

static cJSON *s_tools;

void mcp_init(void) { s_tools = cJSON_Parse(TOOLS_JSON); }

static cJSON *ok(const cJSON *id, cJSON *result)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON_AddItemToObject(r, "result", result);
    return r;
}

static cJSON *error(const cJSON *id, int code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

static cJSON *call_tool(const char *name, const cJSON *args, berr_t *e)
{
    if ((!strcmp(name, "measure") || !strcmp(name, "configure")) && cJSON_GetObjectItemCaseSensitive(args, "force")) {
        snprintf(e->code, sizeof e->code, "bad_request");
        snprintf(e->message, sizeof e->message, "force is not available to agents: ask the user to move the "
                 "lead and update the lead setting on the dashboard");
        return NULL;
    }
    if (!strcmp(name, "measure")) return bridge_measure(args, e);
    if (!strcmp(name, "configure")) return bridge_config(args, e);
    if (!strcmp(name, "get_reading")) {
        for (const cJSON *it = args->child; it; it = it->next) {
            if (strcmp(it->string, "fresh")) {
                snprintf(e->code, sizeof e->code, "bad_request");
                snprintf(e->message, sizeof e->message, "unknown field(s): %s", it->string);
                return NULL;
            }
        }
        const cJSON *fresh = cJSON_GetObjectItemCaseSensitive(args, "fresh");
        return bridge_reading(cJSON_IsTrue(fresh), 5000, e);
    }
    if (!strcmp(name, "get_state")) {
        cJSON *state = bridge_state(e);
        if (!state) return NULL;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "info", bridge_info());
        cJSON_AddItemToObject(r, "state", state);
        return r;
    }
    return bridge_scpi(cJSON_GetObjectItemCaseSensitive(args, "command"), e);
}

static cJSON *text_block(const cJSON *obj)
{
    cJSON *content = cJSON_CreateArray();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "text");
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_AddStringToObject(t, "text", s ? s : "{}");
    cJSON_free(s);
    cJSON_AddItemToArray(content, t);
    return content;
}

cJSON *mcp_handle(const cJSON *msg)
{
    const cJSON *jsonrpc = cJSON_GetObjectItemCaseSensitive(msg, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsObject(msg) || !cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") || !method) {
        if (cJSON_IsObject(msg) && (cJSON_HasObjectItem(msg, "result") || cJSON_HasObjectItem(msg, "error")))
            return NULL;  // a response to a server request; we never send any
        return error(cJSON_IsObject(msg) ? id : NULL, -32600, "Invalid Request");
    }
    if (!id) return NULL;  // notification (notifications/initialized, cancelled, ...)
    const char *m = cJSON_IsString(method) ? method->valuestring : "";
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    if (!strcmp(m, "initialize")) {
        const cJSON *req = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
        const char *version = SUPPORTED_VERSIONS[0];
        for (size_t i = 0; i < sizeof SUPPORTED_VERSIONS / sizeof SUPPORTED_VERSIONS[0]; i++)
            if (cJSON_IsString(req) && !strcmp(req->valuestring, SUPPORTED_VERSIONS[i])) version = SUPPORTED_VERSIONS[i];
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "protocolVersion", version);
        cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
        cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "tools"), "listChanged", false);
        cJSON *si = cJSON_AddObjectToObject(r, "serverInfo");
        cJSON_AddStringToObject(si, "name", "metermind");
        cJSON_AddStringToObject(si, "title", "MeterMind (XDM1041 multimeter)");
        cJSON_AddStringToObject(si, "version", bridge_version());
        cJSON_AddStringToObject(r, "instructions", INSTRUCTIONS);
        return ok(id, r);
    }
    if (!strcmp(m, "ping")) return ok(id, cJSON_CreateObject());
    if (!strcmp(m, "tools/list")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "tools", cJSON_Duplicate(s_tools, true));
        return ok(id, r);
    }
    if (!strcmp(m, "tools/call")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        bool known = false;
        for (const cJSON *t = s_tools ? s_tools->child : NULL; t; t = t->next) {
            const cJSON *tn = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (cJSON_IsString(name) && !strcmp(tn->valuestring, name->valuestring)) known = true;
        }
        if (!known) {
            char buf[96];
            snprintf(buf, sizeof buf, "Unknown tool: %s", cJSON_IsString(name) ? name->valuestring : "None");
            return error(id, -32602, buf);
        }
        cJSON *empty = NULL;
        if (!args || cJSON_IsNull(args)) args = empty = cJSON_CreateObject();
        if (!cJSON_IsObject(args)) return error(id, -32602, "arguments must be an object");
        berr_t e = {0};
        cJSON *result = call_tool(name->valuestring, args, &e);
        cJSON_Delete(empty);
        cJSON *r = cJSON_CreateObject();
        if (!result) {
            cJSON *body = bridge_error_body(&e);
            cJSON_AddItemToObject(r, "content", text_block(body));
            cJSON_AddBoolToObject(r, "isError", true);
            cJSON_Delete(body);
        } else {
            cJSON_AddItemToObject(r, "content", text_block(result));
            cJSON_AddItemToObject(r, "structuredContent", result);
            cJSON_AddBoolToObject(r, "isError", false);
        }
        return ok(id, r);
    }
    char buf[96];
    snprintf(buf, sizeof buf, "Method not found: %s", m);
    return error(id, -32601, buf);
}
