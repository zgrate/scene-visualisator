#include "mcp/server.h"

#include <asio.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace sv::mcp {

namespace {
using asio::ip::tcp;
using Clock = std::chrono::steady_clock;

constexpr std::size_t max_head = 64 * 1024, max_body = 4 * 1024 * 1024;
const std::vector<std::string> versions{"2025-11-25", "2025-06-18", "2025-03-26"}; // newest first

std::string random_hex(int bytes) {
    static std::mutex m;
    static std::random_device device;
    std::lock_guard lock(m);
    std::string s;
    for (int i = 0; i < bytes; ++i)
        s += "0123456789abcdef"[device() % 16], s += "0123456789abcdef"[device() % 16];
    return s;
}

// Compares without an early exit, so the time taken tells nothing about the token.
bool same(const std::string &a, const std::string &b) {
    unsigned diff = a.size() != b.size();
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= unsigned(a[i]) ^ unsigned(i < b.size() ? b[i] : 0);
    return diff == 0;
}

// Browsers send Origin; only pages served from this machine may call (the DNS-rebinding rule of the transport spec).
bool local_origin(const std::string &origin) {
    const auto scheme = origin.find("://");
    if (scheme == std::string::npos)
        return false; // "null" and other opaque origins
    std::string host = origin.substr(scheme + 3);
    if (host.starts_with('['))
        host = host.substr(0, host.find(']') + 1);
    else
        host = host.substr(0, host.find(':'));
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

Response json_response(int status, const json &body) {
    return {status, {{"Content-Type", "application/json"}}, body.dump()};
}

json rpc_error(const json &id, int code, const std::string &message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

const char *reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    default:
        return "Error";
    }
}

std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
} // namespace

json text_result(const json &value) {
    json r = {{"content", json::array({{{"type", "text"},
                                        {"text", value.is_string() ? value.get<std::string>() : value.dump(1)}}})}};
    if (value.is_object())
        r["structuredContent"] = value;
    return r;
}

json image_result(const std::string &png_base64, const json &info) {
    json content = json::array({{{"type", "image"}, {"data", png_base64}, {"mimeType", "image/png"}}});
    if (!info.is_null())
        content.push_back({{"type", "text"}, {"text", info.dump()}});
    return {{"content", std::move(content)}};
}

struct Server::Impl {
    Server *owner = nullptr;
    asio::io_context io;
    std::optional<tcp::acceptor> acceptor;
    std::thread thread;

    mutable std::mutex mutex; // everything below
    std::shared_ptr<const std::vector<Tool>> tools = std::make_shared<std::vector<Tool>>();
    std::shared_ptr<const std::vector<Resource>> resources = std::make_shared<std::vector<Resource>>();
    std::string name = "mcp", version = "0", instructions, token;
    std::function<void(const Call &)> on_call;
    std::map<std::string, Clock::time_point> sessions;
    std::uint64_t calls = 0;
    std::string last_tool;
    std::optional<Clock::time_point> last_call;
    std::uint16_t port = 0;

    void accept();
};

namespace {
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(tcp::socket socket, Server &server) : socket_(std::move(socket)), server_(server) {}
    void start() { read_head(); }

  private:
    void read_head() {
        asio::async_read_until(socket_, buffer_, "\r\n\r\n",
                               [self = shared_from_this()](std::error_code ec, std::size_t n) {
                                   if (!ec)
                                       self->parse_head(n);
                               });
    }

    void parse_head(std::size_t n) {
        std::string head(asio::buffers_begin(buffer_.data()), asio::buffers_begin(buffer_.data()) + std::ptrdiff_t(n));
        buffer_.consume(n);
        request_ = {};
        std::size_t at = head.find("\r\n");
        std::istringstream first(head.substr(0, at));
        std::string protocol;
        first >> request_.method >> request_.target >> protocol;
        while (at != std::string::npos && at + 2 < head.size()) {
            const std::size_t end = head.find("\r\n", at + 2);
            const std::string line = head.substr(at + 2, end - at - 2);
            if (const auto colon = line.find(':'); colon != std::string::npos)
                request_.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            at = end;
        }
        keep_alive_ = protocol == "HTTP/1.1" && lower(header("connection")) != "close";
        if (!header("transfer-encoding").empty())
            return reply({411, {}, {}}); // shortcut: no chunked bodies, which MCP clients do not send
        std::size_t length = 0;
        try {
            length = header("content-length").empty() ? 0 : std::stoul(header("content-length"));
        } catch (const std::exception &) {
            return reply({400, {}, {}});
        }
        if (length > max_body)
            return keep_alive_ = false, reply({413, {}, {}});
        const std::size_t have = std::min(length, buffer_.size());
        request_.body.assign(asio::buffers_begin(buffer_.data()),
                             asio::buffers_begin(buffer_.data()) + std::ptrdiff_t(have));
        buffer_.consume(have);
        if (have == length)
            return reply(server_.handle(request_));
        request_.body.resize(length);
        asio::async_read(socket_, asio::buffer(request_.body.data() + have, length - have),
                         [self = shared_from_this()](std::error_code ec, std::size_t) {
                             if (!ec)
                                 self->reply(self->server_.handle(self->request_));
                         });
    }

    std::string header(const char *name) const {
        const auto it = request_.headers.find(name);
        return it == request_.headers.end() ? std::string() : it->second;
    }

    void reply(Response r) {
        out_ = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
        r.headers["Content-Length"] = std::to_string(r.body.size());
        if (!keep_alive_)
            r.headers["Connection"] = "close";
        for (const auto &[k, v] : r.headers)
            out_ += k + ": " + v + "\r\n";
        out_ += "\r\n" + r.body;
        asio::async_write(socket_, asio::buffer(out_), [self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!ec && self->keep_alive_)
                self->read_head();
            else
                self->socket_.close(ec);
        });
    }

    tcp::socket socket_;
    Server &server_;
    asio::streambuf buffer_{max_head + max_body};
    Request request_;
    std::string out_;
    bool keep_alive_ = true;
};
} // namespace

Server::Server() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Server::~Server() { stop(); }

void Server::set_tools(std::vector<Tool> tools) {
    std::lock_guard lock(impl_->mutex);
    impl_->tools = std::make_shared<std::vector<Tool>>(std::move(tools));
}

void Server::set_resources(std::vector<Resource> resources) {
    std::lock_guard lock(impl_->mutex);
    impl_->resources = std::make_shared<std::vector<Resource>>(std::move(resources));
}

void Server::set_info(std::string name, std::string version, std::string instructions) {
    std::lock_guard lock(impl_->mutex);
    impl_->name = std::move(name), impl_->version = std::move(version), impl_->instructions = std::move(instructions);
}

void Server::on_call(std::function<void(const Call &)> fn) {
    std::lock_guard lock(impl_->mutex);
    impl_->on_call = std::move(fn);
}

void Server::start(const Config &config) {
    stop();
    tcp::acceptor acceptor(impl_->io);
    const tcp::endpoint endpoint(asio::ip::make_address(config.bind), config.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint); // throws std::system_error, e.g. port in use
    acceptor.listen();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->token = config.token;
        impl_->port = acceptor.local_endpoint().port();
        impl_->sessions.clear();
    }
    impl_->acceptor.emplace(std::move(acceptor));
    impl_->io.restart();
    impl_->accept();
    impl_->thread = std::thread([this] { impl_->io.run(); });
}

void Server::Impl::accept() {
    acceptor->async_accept([this](std::error_code ec, tcp::socket socket) {
        if (ec)
            return; // closed by stop()
        std::make_shared<Connection>(std::move(socket), *owner)->start();
        accept();
    });
}

void Server::stop() {
    if (!impl_->thread.joinable())
        return;
    asio::post(impl_->io, [this] {
        std::error_code ec;
        impl_->acceptor->close(ec);
        impl_->io.stop(); // drops open connections with it
    });
    impl_->thread.join();
    impl_->acceptor.reset();
    std::lock_guard lock(impl_->mutex);
    impl_->port = 0;
}

Status Server::status() const {
    std::lock_guard lock(impl_->mutex);
    Status s;
    s.listening = impl_->port != 0;
    s.port = impl_->port;
    const auto now = Clock::now();
    for (const auto &[id, seen] : impl_->sessions)
        s.clients += now - seen < std::chrono::minutes(1);
    s.calls = impl_->calls;
    s.last_tool = impl_->last_tool;
    if (impl_->last_call)
        s.seconds_since_call = std::chrono::duration<double>(now - *impl_->last_call).count();
    return s;
}

Response Server::handle(const Request &request) {
    const auto header = [&](const char *name) {
        const auto it = request.headers.find(name);
        return it == request.headers.end() ? std::string() : it->second;
    };
    std::string token, name, version, instructions;
    std::shared_ptr<const std::vector<Tool>> tools;
    std::shared_ptr<const std::vector<Resource>> resources;
    {
        std::lock_guard lock(impl_->mutex);
        token = impl_->token, name = impl_->name, version = impl_->version, instructions = impl_->instructions;
        tools = impl_->tools, resources = impl_->resources;
    }

    if (request.target.substr(0, request.target.find('?')) != "/mcp")
        return {404, {}, {}};
    if (const std::string origin = header("origin"); !origin.empty() && !local_origin(origin))
        return {403, {}, {}};
    if (!token.empty() && !same(header("authorization"), "Bearer " + token))
        return {401, {{"WWW-Authenticate", "Bearer realm=\"Scene Visualiser\""}}, {}};
    const std::string session = header("mcp-session-id");
    if (request.method == "DELETE") {
        std::lock_guard lock(impl_->mutex);
        return {impl_->sessions.erase(session) ? 200 : 404, {}, {}};
    }
    if (request.method != "POST") // no server-to-client stream (shortcut: no push; add SSE when a client needs it)
        return {405, {{"Allow", "POST, DELETE"}}, {}};
    if (const std::string v = header("mcp-protocol-version");
        !v.empty() && std::ranges::find(versions, v) == versions.end())
        return json_response(400, rpc_error(nullptr, -32600, "Unsupported MCP-Protocol-Version " + v));

    json message;
    try {
        message = json::parse(request.body);
    } catch (const json::exception &) {
        return json_response(400, rpc_error(nullptr, -32700, "Parse error"));
    }
    if (message.is_array())
        return json_response(400, rpc_error(nullptr, -32600, "JSON-RPC batches are not supported"));
    if (!message.is_object() || message.value("jsonrpc", "") != "2.0")
        return json_response(400, rpc_error(nullptr, -32600, "Invalid request"));
    if (!message.contains("method") || !message.contains("id"))
        return {202, {}, {}}; // a notification or a response to us: nothing to answer
    const json id = message["id"];
    const std::string method = message.value("method", "");
    const json params = message.value("params", json::object());

    Response response{200, {{"Content-Type", "application/json"}}, {}};
    {
        std::lock_guard lock(impl_->mutex);
        if (method == "initialize") {
            const std::string id = random_hex(16);
            impl_->sessions[id] = Clock::now();
            response.headers["Mcp-Session-Id"] = id;
        } else if (!session.empty()) {
            const auto it = impl_->sessions.find(session);
            if (it == impl_->sessions.end()) // e.g. the app restarted: the client starts a new session
                return json_response(404, rpc_error(id, -32001, "Session not found"));
            it->second = Clock::now();
        }
    }
    const auto ok = [&](json result) {
        response.body = json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump();
        return response;
    };
    const auto fail = [&](int code, const std::string &text) {
        response.body = rpc_error(id, code, text).dump();
        return response;
    };
    const auto usable = [](const Tool &t) { return !t.available || t.available(); };

    if (method == "initialize") {
        const std::string asked = params.value("protocolVersion", "");
        json result = {{"protocolVersion", std::ranges::find(versions, asked) != versions.end() ? asked : versions[0]},
                       {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", json::object()}}},
                       {"serverInfo", {{"name", name}, {"version", version}}}};
        if (!instructions.empty())
            result["instructions"] = instructions;
        return ok(std::move(result));
    }
    if (method == "ping")
        return ok(json::object());
    if (method == "tools/list") {
        json list = json::array();
        for (const Tool &t : *tools)
            if (usable(t))
                list.push_back({{"name", t.name},
                                {"description", t.description},
                                {"inputSchema", t.input_schema},
                                {"annotations", {{"readOnlyHint", t.read_only}}}});
        return ok({{"tools", std::move(list)}});
    }
    if (method == "tools/call") {
        const std::string tool = params.value("name", "");
        const auto it = std::ranges::find_if(*tools, [&](const Tool &t) { return t.name == tool && usable(t); });
        if (it == tools->end())
            return fail(-32602, "Unknown tool: " + tool);
        Call call{tool, params.value("arguments", json::object())};
        const auto start = Clock::now();
        json result;
        try {
            if (!call.arguments.is_object())
                call.arguments = json::object();
            // A plain error for a missing required argument, not nlohmann's out_of_range text from args.at().
            if (const auto req = it->input_schema.find("required"); req != it->input_schema.end() && req->is_array())
                for (const json &k : *req)
                    if (k.is_string() && !call.arguments.contains(k.get<std::string>()))
                        throw std::invalid_argument("Missing argument: " + k.get<std::string>());
            result = it->run(call.arguments);
        } catch (const std::exception &e) {
            call.ok = false, call.error = e.what();
            result = {{"content", json::array({{{"type", "text"}, {"text", e.what()}}})}, {"isError", true}};
        }
        call.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::function<void(const Call &)> notify;
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->calls, impl_->last_tool = tool, impl_->last_call = Clock::now();
            notify = impl_->on_call;
        }
        if (notify)
            notify(call);
        return ok(std::move(result));
    }
    if (method == "resources/list") {
        json list = json::array();
        for (const Resource &r : *resources)
            list.push_back({{"uri", r.uri}, {"name", r.name}, {"mimeType", r.mime_type}});
        return ok({{"resources", std::move(list)}});
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        for (const Resource &r : *resources)
            if (r.uri == uri)
                try {
                    return ok(
                        {{"contents", json::array({{{"uri", uri}, {"mimeType", r.mime_type}, {"text", r.read()}}})}});
                } catch (const std::exception &e) {
                    return fail(-32603, e.what());
                }
        return fail(-32002, "Resource not found: " + uri);
    }
    return fail(-32601, "Method not found: " + method);
}

} // namespace sv::mcp
