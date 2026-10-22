#pragma once
// MCP (Model Context Protocol, spec 2025-11-25) over Streamable HTTP: one POST /mcp endpoint answering plain JSON,
// no SSE stream. Runs its own Asio thread; tools run on it unless their callback sends them elsewhere. No Qt.
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sv::mcp {

using json = nlohmann::json;

struct Tool {
    std::string name, description;
    json input_schema = {{"type", "object"}};
    bool read_only = false;
    // Returns an MCP tool result ({content, structuredContent?}); an exception becomes an isError result.
    std::function<json(const json &args)> run;
    std::function<bool()> available; // listed and callable only while true; empty: always
};
json text_result(const json &value); // the value as JSON text, and as structuredContent when it is an object
json image_result(const std::string &png_base64, const json &info = {});

struct Resource {
    std::string uri, name, mime_type;
    std::function<std::string()> read;
};

struct Config {
    std::string bind = "127.0.0.1"; // 0.0.0.0 for LAN access
    std::uint16_t port = 7311;      // 0: ephemeral (tests)
    std::string token;              // required as "Authorization: Bearer <token>"; empty: no check (tests only)
};

struct Request {
    std::string method, target;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
};
struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct Call {
    std::string tool;
    json arguments;
    double ms = 0;
    bool ok = true;
    std::string error;
};

struct Status {
    bool listening = false;
    std::uint16_t port = 0;
    int clients = 0;                // sessions seen in the last minute
    std::uint64_t calls = 0;        // tool calls since start
    std::string last_tool;          // empty before the first call
    double seconds_since_call = -1; // -1 before the first call
};

class Server {
  public:
    Server();
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    void set_tools(std::vector<Tool> tools);
    void set_resources(std::vector<Resource> resources);
    void set_info(std::string name, std::string version, std::string instructions);
    void on_call(std::function<void(const Call &)> fn); // after every tool call, on the server thread

    void start(const Config &config); // throws std::system_error if it cannot bind; restarts when running
    void stop();
    Status status() const;

    Response handle(const Request &request); // the HTTP exchange without sockets, also for tests

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sv::mcp
