# AI access: MCP over Streamable HTTP on its own Asio thread. No Qt.
add_library(sv_mcp STATIC server.cpp server.h)
target_include_directories(sv_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sv_mcp PUBLIC nlohmann_json::nlohmann_json PRIVATE asio)
