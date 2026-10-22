#pragma once
// AI access (docs/mcp-plan.md): the MCP tool table over Session. Tools hop to the UI thread and wait up to 10 s.
#include "mcp/server.h"

#include <vector>

class Session;

std::vector<sv::mcp::Tool> make_mcp_tools(Session &s);
std::vector<sv::mcp::Resource> make_mcp_resources(Session &s);
extern const char *const mcp_instructions;
