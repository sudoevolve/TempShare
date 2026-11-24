#include "core/http_server.hpp"

#include "core/session.hpp"
#include "web_assets.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <string_view>

namespace tempshare {
namespace {

using httplib::Request;
using httplib::Response;
constexpr auto json = "application/json; charset=utf-8";

void fail(Response& response, int status, std::string_view message) {
    response.status = status;
    response.set_content("{\"error\":" + json_string(message) + "}", json);
}

std::string cookie_token(const Request& request, const std::string& name) {
    const std::string cookies = request.get_header_value("Cookie");
    std::string_view rest = cookies;
    while (!rest.empty()) {
        const auto separator = rest.find(';');
        auto segment = rest.substr(0, separator);
        const auto start = segment.find_first_not_of(" \t");
        if (start != std::string_view::npos) {
            segment.remove_prefix(start);
            if (segment.substr(0, name.size()) == name && segment.substr(name.size(), 1) == "=") {
                return std::string(segment.substr(name.size() + 1));
            }
        }
        if (separator == std::string_view::npos) break;
        rest.remove_prefix(separator + 1);
    }
    return {};
}

std::string offer_json(const Offer& offer) {
    return "{\"id\":" + json_string(offer.id) + ",\"state\":" + json_string(state_name(offer.state)) + "}";
}

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return {};
    return value;
}

std::optional<std::string> decode_filename(std::string_view text) {
    std::string name;
    if (text.size() > 3 * 240) return {};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            name += text[i];
            continue;
        }
        if (i + 2 >= text.size()) return {};
        unsigned value = 0;
        const auto end = text.data() + i + 3;
        const auto parsed = std::from_chars(text.data() + i + 1, end, value, 16);
        if (parsed.ec != std::errc() || parsed.ptr != end) return {};
        name += static_cast<char>(value);
        i += 2;
    }
    if (!valid_filename(name)) return {};
    return name;
}

void send_file(Session& session, const std::string& token, const File& file, Response& response) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file.path, error);
    auto input = std::make_shared<std::ifstream>(file.path, std::ios::binary);
    if (error || !*input || size != file.size) {
        return fail(response, 409, "File changed or is unavailable");
    }
    response.set_header("Content-Disposition", "attachment; filename*=UTF-8''" +
                        httplib::encode_uri_component(file.name));
    response.set_content_provider(static_cast<std::size_t>(size), "application/octet-stream",
        [&session, input, token, id = file.id](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            if (!session.downloading(token, id)) return false;
            std::array<char, 64 * 1024> chunk{};
            const auto count = std::min(length, chunk.size());
            input->seekg(static_cast<std::streamoff>(offset));
            input->read(chunk.data(), static_cast<std::streamsize>(count));
            return *input && sink.write(chunk.data(), count);
        });
}

struct UploadCleanup {
    Session& session;
    const std::string& token;
    const std::string& id;
    const Upload& file;
    bool committed = false;

    ~UploadCleanup() {
        if (committed) return;
        std::error_code error;
        std::filesystem::remove(file.temporary, error);
        session.retry_upload(token, id);
    }
};

}  // namespace

HttpServer::HttpServer(Session& session, const std::string& certificate, const std::string& key)
    : secure_(!certificate.empty() || !key.empty()) {
    if (secure_) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        server_ = std::make_unique<httplib::SSLServer>(certificate.c_str(), key.c_str());
#else
        return;
#endif
    } else {
        server_ = std::make_unique<httplib::Server>();
    }
    auto& server = *server_;
    server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 32); };
    server.set_payload_max_length(max_file_size);
    server.set_keep_alive_max_count(1);
    server.set_read_timeout(10);
    server.set_write_timeout(10);
    server.set_default_headers({
        {"Cache-Control", "no-store"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Content-Type-Options", "nosniff"},
        {"Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; "
                                    "connect-src 'self'; img-src 'self'; base-uri 'none'; "
                                    "frame-ancestors 'none'; form-action 'self'"}
    });
    const auto auth_cookie = [this] { return "ts_" + std::to_string(port_); };
    const auto auth = [auth_cookie](const Request& request) {
        return cookie_token(request, auth_cookie());
    };

    server.set_pre_request_handler([this](const Request& request, Response& response) {
        using Result = httplib::Server::HandlerResponse;
        const bool loopback = request.local_addr == "127.0.0.1" && request.remote_addr == "127.0.0.1";
        const auto interfaces = networks();
        if (!loopback && std::none_of(interfaces.begin(), interfaces.end(), [&](const LocalNetwork& network) {
            return network.address == request.local_addr && network.contains(request.remote_addr);
        })) {
            fail(response, 403, "Local subnet required");
            return Result::Handled;
        }
        const auto header = request.get_header_value("Host");
        const auto host = header.substr(0, header.find(':'));
        if (host != request.local_addr && !(loopback && host == "localhost")) {
            fail(response, 400, "Open the IPv4 address shown in TempShare");
            return Result::Handled;
        }
        if (request.method != "GET" && request.method != "HEAD" && request.get_header_value("X-TempShare") != "1") {
            fail(response, 403, "Request header missing");
            return Result::Handled;
        }
        constexpr std::string_view upload_prefix = "/api/upload/";
        const std::string_view path = request.path;
        const bool upload = request.method == "PUT" && path.substr(0, upload_prefix.size()) == upload_prefix &&
            path.size() == upload_prefix.size() + 32 &&
            path.substr(upload_prefix.size()).find_first_not_of("0123456789abcdef") == std::string_view::npos;
        if (!upload && (request.has_header("Transfer-Encoding") ||
            (request.has_header("Content-Length") && request.get_header_value("Content-Length") != "0"))) {
            fail(response, 400, "Unexpected request body");
            return Result::Handled;
        }
        return Result::Unhandled;
    });

    server.Get("/", [](const Request&, Response& response) { response.set_content(web::index, "text/html; charset=utf-8"); });
    server.Get("/styles.css", [](const Request&, Response& response) { response.set_content(web::styles, "text/css; charset=utf-8"); });
    server.Get("/app.js", [](const Request&, Response& response) { response.set_content(web::script, "text/javascript; charset=utf-8"); });
    server.Get("/api/health", [](const Request&, Response& response) { response.set_content("{\"service\":\"tempshare\"}", json); });

    server.Post("/api/pair", [this, &session, auth_cookie](const Request& request, Response& response,
                                                          const httplib::ContentReader&) {
        const auto pairing = session.pair(request.get_header_value("X-Invitation"));
        if (!pairing) return fail(response, 403, "Invitation unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=" + pairing->token +
            "; Path=/; HttpOnly; SameSite=Strict" + (secure_ ? "; Secure" : ""));
        response.status = 201;
        response.set_content("{\"verification\":" + json_string(pairing->verification) + "}", json);
    });
    server.Get("/api/pair", [&session, auth](const Request& request, Response& response) {
        const auto pairing = session.pairing(auth(request));
        if (!pairing) return fail(response, 401, "Session expired or denied");
        response.set_content("{\"state\":" + json_string(state_name(pairing->state)) +
                             ",\"verification\":" + json_string(pairing->verification) + "}", json);
    });

    server.Get("/api/files", [&session, auth](const Request& request, Response& response) {
        const auto files = session.shared(auth(request));
        if (!files) return fail(response, 403, "Connection approval required");
        std::string body = "[";
        for (const auto& file : *files) {
            if (body.size() > 1) body += ',';
            body += "{\"id\":" + json_string(file.id) + ",\"name\":" + json_string(file.name) +
                    ",\"size\":" + std::to_string(file.size) + '}';
        }
        response.set_content(body + ']', json);
    });
    server.Get(R"(/api/download/([0-9a-f]{32}))", [&session, auth](const Request& request, Response& response) {
        const auto token = auth(request);
        const auto files = session.shared(token);
        if (!files) return fail(response, 403, "Connection approval required");
        for (const auto& file : *files) {
            if (file.id == request.matches[1].str()) return send_file(session, token, file, response);
        }
        fail(response, 404, "Shared file not found");
    });

    server.Post("/api/offers", [&session, auth](const Request& request, Response& response,
                                              const httplib::ContentReader&) {
        const auto token = auth(request);
        if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
        auto name = decode_filename(request.get_header_value("X-File-Name"));
        const auto size = number(request.get_header_value("X-File-Size"));
        if (!name || !size || *size > max_file_size) return fail(response, 400, "Invalid file name or size");
        const auto offer = session.offer(token, std::move(*name), *size);
        if (!offer) return fail(response, 409, "Unable to create a new request");
        response.status = 201;
        response.set_content(offer_json(*offer), json);
    });
    server.Get("/api/offers", [&session, auth](const Request& request, Response& response) {
        const auto offers = session.offers(auth(request));
        if (!offers) return fail(response, 403, "Connection approval required");
        std::string body = "[";
        for (const auto& offer : *offers) {
            if (body.size() > 1) body += ',';
            body += offer_json(offer);
        }
        response.set_content(body + ']', json);
    });
    server.Delete(R"(/api/offers/([0-9a-f]{32}))", [&session, auth](const Request& request, Response& response,
                                                                 const httplib::ContentReader&) {
        if (!session.cancel_offer(auth(request), request.matches[1].str())) {
            return fail(response, 409, "Request no longer available");
        }
        response.status = 204;
    });
    server.Put(R"(/api/upload/([0-9a-f]{32}))", [&session, auth](const Request& request, Response& response,
                                                               const httplib::ContentReader& read) {
        const auto size = number(request.get_header_value("Content-Length"));
        if (!size || *size > max_file_size || request.has_header("Content-Encoding") ||
            request.has_header("Transfer-Encoding")) {
            return fail(response, 400, "Invalid upload size or encoding");
        }
        const auto token = auth(request);
        const auto id = request.matches[1].str();
        const auto file = session.begin_upload(token, id, *size);
        if (!file) return fail(response, 409, "Upload approval or writable receive folder required");
        UploadCleanup cleanup{session, token, id, *file};
        std::ofstream output(file->temporary, std::ios::binary);
        if (!output) return fail(response, 507, "Unable to open receive file");

        std::uint64_t written = 0;
        if (!read([&](const char* data, std::size_t length) {
            if (length > *size - written || !session.uploading(token, id)) return false;
            output.write(data, static_cast<std::streamsize>(length));
            written += length;
            return static_cast<bool>(output);
        }) || written != *size) return fail(response, 400, "Upload interrupted");
        output.close();
        if (!output || !session.finish_upload(token, id, *file)) {
            return fail(response, 409, "Upload could not be saved");
        }
        cleanup.committed = true;
        response.status = 201;
        response.set_content("{\"state\":\"complete\"}", json);
    });
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(unsigned short preferred_port, bool loopback) {
    if (thread_.joinable() || !server_ || !server_->is_valid()) return false;
    update_networks(loopback ? std::vector<LocalNetwork>{} : lan_networks());
    const auto* address = loopback ? "127.0.0.1" : "0.0.0.0";
    if (preferred_port && server_->bind_to_port(address, preferred_port)) {
        port_ = preferred_port;
    } else {
        const int port = server_->bind_to_any_port(address);
        if (port <= 0) return false;
        port_ = static_cast<unsigned short>(port);
    }
    thread_ = std::thread([this] {
        server_->listen_after_bind();
        server_->decommission();
    });
    server_->wait_until_ready();
    return server_->is_running();
}

void HttpServer::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

std::string HttpServer::url(const std::string& address) const {
    return std::string(secure_ ? "https://" : "http://") + address + ':' + std::to_string(port_) + '/';
}

std::vector<LocalNetwork> HttpServer::networks() const {
    std::lock_guard lock(networks_mutex_);
    return networks_;
}

bool HttpServer::update_networks(std::vector<LocalNetwork> networks) {
    std::lock_guard lock(networks_mutex_);
    if (networks_ == networks) return false;
    networks_ = std::move(networks);
    return true;
}

}  // namespace tempshare
