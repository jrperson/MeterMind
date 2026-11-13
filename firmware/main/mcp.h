// Minimal MCP server: Streamable HTTP transport, stateless, JSON responses only (no SSE).
// Port of bridge/mcp.py; the HTTP side lives in http.c.
#pragma once

#include "cJSON.h"

void mcp_init(void);
// Handles one JSON-RPC message. Returns the response, or NULL for notifications and responses.
cJSON *mcp_handle(const cJSON *msg);
