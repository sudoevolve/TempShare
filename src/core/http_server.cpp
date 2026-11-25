#include "core/http_server.hpp"

#include "core/session.hpp"
#include "web_assets.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace tempshare {
namespace {

using httplib::Request;
using httplib::Response;
constexpr auto json = "application/json; charset=utf-8";
constexpr auto trust_cookie = "ts_trusted";
constexpr std::size_t max_sdp_size = 64 * 1024;

const char* boolean(bool value) { return value ? "true" : "false"; }

bool local_socket(const Request& request) {
    return request.local_addr == "127.0.0.1" && request.remote_addr == "127.0.0.1";
}

void fail(Response& response, int status, std::string_view message, std::string_view code = {}) {
    response.status = status;
    auto body = "{\"error\":" + json_string(message);
    if (!code.empty()) body += ",\"code\":" + json_string(code);
    response.set_content(body + '}', json);
}

class TransferSlots : public std::enable_shared_from_this<TransferSlots> {
public:
    std::shared_ptr<const void> acquire() {
        auto count = active_.load();
        while (count < 4) {
            if (active_.compare_exchange_weak(count, count + 1)) {
                return {this, [owner = shared_from_this()](const void*) { --owner->active_; }};
            }
        }
        return {};
    }

private:
    std::atomic<unsigned> active_{0};
};

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
    return "{\"id\":" + json_string(offer.id) + ",\"state\":" + json_string(state_name(offer.state)) +
        ",\"transferred\":" + std::to_string(offer.transferred) + "}";
}

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return {};
    return value;
}

std::string cast_json(const CastSignal& signal) {
    return "{\"offer\":" + json_string(signal.offer) + ",\"answer\":" + json_string(signal.answer) +
        ",\"viewing\":" + boolean(signal.viewing) + ",\"revision\":" + std::to_string(signal.revision) +
        ",\"ready\":" + boolean(signal.ready) + "}";
}

// mDNS host candidates remain opaque to the service. Numeric candidates must match a local subnet.
bool lan_address(const std::string& address, const std::vector<LocalNetwork>& networks) {
    if (address.size() > 6 && address.substr(address.size() - 6) == ".local") {
        return address.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") ==
            std::string_view::npos;
    }
    if (networks.empty()) return is_private_ipv4(address);
    return std::any_of(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
        return network.contains(address);
    });
}

std::string lan_sdp(const std::string& body, const std::vector<LocalNetwork>& networks) {
    if (body.rfind("v=0", 0) != 0 || body.find('\0') != std::string::npos) return {};
    std::istringstream stream(body);
    std::string line;
    std::string result;
    bool candidate = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("a=candidate:", 0) == 0) {
            std::istringstream fields(line);
            std::string foundation, component, protocol, priority, address, port, type, host;
            if (!(fields >> foundation >> component >> protocol >> priority >> address >> port >> type >> host) ||
                type != "typ" || host != "host" || !lan_address(address, networks)) continue;
            candidate = true;
        }
        if (line.rfind("c=IN ", 0) == 0) line = "c=IN IP4 0.0.0.0";
        if (line.rfind("a=rtcp:", 0) == 0 || line.rfind("a=remote-candidates:", 0) == 0) continue;
        result += line + "\r\n";
    }
    return candidate ? result : std::string{};
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

void send_file(Session& session, const std::string& token, const File& file, Response& response,
               const std::shared_ptr<TransferSlots>& slots) {
    const auto slot = slots->acquire();
    if (!slot) {
        response.set_header("Retry-After", "1");
        return fail(response, 503, "File transfer slots are busy", "transfer_busy");
    }
    auto input = std::make_shared<SharedSource>(file.path);
    if (!file.source || !(input->version() == file.source)) {
        return fail(response, 409, "File changed or is unavailable", "file_changed");
    }
    const auto deadline = Session::Clock::now() + std::chrono::minutes(5);
    response.set_header("Content-Disposition", "attachment; filename*=UTF-8''" +
                        httplib::encode_uri_component(file.name));
    response.set_content_provider(static_cast<std::size_t>(file.size), "application/octet-stream",
        [&session, directory = file.temporary_directory, input, id = file.id, version = file.source,
         token, deadline](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            if (Session::Clock::now() >= deadline || !session.downloading(token, id)) return false;
            std::array<char, 64 * 1024> chunk{};
            const auto count = std::min(length, chunk.size());
            return input->read(offset, chunk.data(), count) && input->version() == version &&
                sink.write(chunk.data(), count);
        }, [slot](bool) {});
}

struct UploadCleanup {
    Session& session;
    const std::string& token;
    const std::string& id;
    bool committed = false;

    ~UploadCleanup() {
        if (committed) return;
        session.retry_upload(token, id);
    }
};

using CookieName = std::function<std::string()>;
using Authorize = std::function<std::string(const Request&)>;

void register_pages(httplib::Server& server) {
    server.Get("/", [](const Request&, Response& response) { response.set_content(web::index, "text/html; charset=utf-8"); });
    server.Get("/styles.css", [](const Request&, Response& response) { response.set_content(web::styles, "text/css; charset=utf-8"); });
    server.Get("/app.js", [](const Request&, Response& response) { response.set_content(web::script, "text/javascript; charset=utf-8"); });
    server.Get("/rtc.js", [](const Request&, Response& response) { response.set_content(web::rtc, "text/javascript; charset=utf-8"); });
    server.Get("/resume.js", [](const Request&, Response& response) { response.set_content(web::resume, "text/javascript; charset=utf-8"); });
    server.Get("/cast-host.js", [](const Request&, Response& response) { response.set_content(web::cast_host, "text/javascript; charset=utf-8"); });
    server.Get("/cast", [](const Request& request, Response& response) {
        if (!local_socket(request)) return fail(response, 403, "Open the cast console on the desktop");
        response.set_content(web::cast_page, "text/html; charset=utf-8");
    });
    server.Get("/api/health", [](const Request&, Response& response) { response.set_content("{\"service\":\"tempshare\"}", json); });
}

void register_pairing(httplib::Server& server, Session& session, bool secure,
                      const CookieName& auth_cookie, const Authorize& auth) {
    server.Post("/api/pair", [secure, &session, auth_cookie](const Request& request, Response& response,
                                                          const httplib::ContentReader&) {
        const auto name = decode_filename(request.get_header_value("X-Device-Name"));
        const auto pairing = session.pair(request.get_header_value("X-Invitation"), name.value_or("浏览器"));
        if (!pairing) return fail(response, 403, "Invitation unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=" + pairing->token +
            "; Path=/; HttpOnly; SameSite=Strict" + (secure ? "; Secure" : ""));
        response.status = 201;
        response.set_content("{\"verification\":" + json_string(pairing->verification) + "}", json);
    });
    server.Post("/api/pair/trusted", [secure, &session, auth_cookie](const Request& request, Response& response,
                                                                   const httplib::ContentReader&) {
        if (!secure) return fail(response, 403, "HTTPS is required for remembered devices");
        const auto pairing = session.reconnect(cookie_token(request, trust_cookie));
        if (!pairing) return fail(response, 403, "Remembered device or active session unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=" + pairing->token + "; Path=/; HttpOnly; Secure; SameSite=Strict");
        response.status = 201;
    });
    server.Get("/api/pair", [secure, &session, auth](const Request& request, Response& response) {
        const auto pairing = session.pairing(auth(request));
        if (!pairing) return fail(response, 401, "Session expired or denied");
        const bool trusted = secure && !pairing->trusted_key.empty();
        if (trusted && cookie_token(request, trust_cookie) != pairing->trusted_key) {
            response.set_header("Set-Cookie", std::string(trust_cookie) + "=" + pairing->trusted_key +
                "; Path=/api/pair; Max-Age=31536000; HttpOnly; Secure; SameSite=Strict");
        }
        const auto permissions = session.permissions();
        response.set_content("{\"state\":" + json_string(state_name(pairing->state)) +
            ",\"verification\":" + json_string(pairing->verification) + ",\"download\":" + boolean(permissions.download) +
            ",\"upload\":" + boolean(permissions.upload) + ",\"text\":" + boolean(permissions.text) +
            ",\"trusted\":" + boolean(trusted) + ",\"textRevision\":" +
            std::to_string(session.text_revision()) + ",\"castReady\":" +
            boolean(session.capture_ready(pairing->token).value_or(false)) + ",\"filesRevision\":" +
            std::to_string(session.files_revision()) + ",\"offersRevision\":" +
            std::to_string(session.offers_revision()) + "}", json);
    });
    server.Delete("/api/pair", [secure, &session, auth, auth_cookie](const Request& request, Response& response,
                                                            const httplib::ContentReader&) {
        if (!session.disconnect(auth(request))) return fail(response, 401, "Session is unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict" +
            (secure ? "; Secure" : ""));
        response.status = 204;
    });

    server.Get("/api/text", [&session, auth](const Request& request, Response& response) {
        const auto messages = session.messages(auth(request));
        if (!messages) return fail(response, 403, "Text sharing is unavailable");
        std::string body = "[";
        for (const auto& message : *messages) {
            if (body.size() > 1) body += ',';
            body += "{\"text\":" + json_string(message.text) + ",\"desktop\":" + boolean(message.from_desktop) + '}';
        }
        response.set_content(body + ']', json);
    });
    server.Post("/api/text", [&session, auth](const Request& request, Response& response) {
        if (!session.authorized(auth(request))) return fail(response, 403, "Connection approval required");
        if (!session.send_text(auth(request), request.body)) return fail(response, 400, "Invalid or disabled text");
        response.status = 201;
    });
}

void register_cast(httplib::Server& server, Session& session, const Authorize& auth,
                   std::function<std::vector<LocalNetwork>()> interfaces) {
    const auto host_key = [](const Request& request) {
        return local_socket(request) ? request.get_header_value("X-Cast-Key") : std::string{};
    };
    server.Get("/api/cast/host", [&session, host_key](const Request& request, Response& response) {
        const auto signal = session.cast_host(host_key(request));
        if (!signal) return fail(response, 403, "Desktop cast authorization required");
        response.set_content(cast_json(*signal), json);
    });
    server.Post("/api/cast/ready", [&session, host_key](const Request& request, Response& response,
                                                       const httplib::ContentReader&) {
        if (!session.cast_ready(host_key(request))) return fail(response, 403, "Desktop cast authorization required");
        response.status = 204;
    });
    server.Post("/api/cast/offer", [&session, host_key, interfaces](const Request& request, Response& response) {
        const auto key = host_key(request);
        if (!session.cast_host(key)) return fail(response, 403, "Desktop cast authorization required");
        auto offer = lan_sdp(request.body, interfaces());
        if (offer.empty() || !session.cast_offer(key, std::move(offer))) return fail(response, 409, "No LAN viewer");
        response.status = 201;
    });
    server.Post("/api/cast/stop", [&session, host_key](const Request& request, Response& response) {
        const auto key = host_key(request);
        if (key.empty() || !session.stop_cast(key)) return fail(response, 403, "Desktop cast authorization required");
        response.status = 204;
    });
    server.Post("/api/cast/viewer", [&session, auth](const Request& request, Response& response) {
        const auto token = auth(request);
        if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
        const auto signal = session.cast_viewer(token, request.get_header_value("X-Cast-Watch") == "1",
                                               request.get_header_value("X-Cast-Viewer"));
        if (!signal) return fail(response, 409, "Another viewer is active", "viewer_busy");
        response.set_content(cast_json(*signal), json);
    });
    server.Post("/api/cast/answer", [&session, auth, interfaces](const Request& request, Response& response) {
        const auto token = auth(request);
        if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
        const auto revision = number(request.get_header_value("X-Cast-Revision"));
        auto networks = interfaces();
        if (!local_socket(request)) {
            networks.erase(std::remove_if(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
                return network.address != request.local_addr;
            }), networks.end());
            if (networks.empty()) return fail(response, 403, "Local subnet required");
        }
        auto answer = lan_sdp(request.body, networks);
        if (!revision || *revision > std::numeric_limits<unsigned>::max() || answer.empty() ||
            !session.cast_answer(token, static_cast<unsigned>(*revision), std::move(answer),
                                 request.get_header_value("X-Cast-Viewer"))) {
            return fail(response, 409, "Cast offer changed or no LAN candidate is available");
        }
        response.status = 201;
    });
}

void register_transfers(httplib::Server& server, Session& session, const Authorize& auth) {
    const auto transfers = std::make_shared<TransferSlots>();
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
    server.Get(R"(/api/download/([0-9a-f]{32}))", [&session, auth, transfers](const Request& request, Response& response) {
        const auto token = auth(request);
        const auto files = session.shared(token);
        if (!files) return fail(response, 403, "Connection approval required");
        for (const auto& file : *files) {
            if (file.id == request.matches[1].str()) return send_file(session, token, file, response, transfers);
        }
        fail(response, 404, "Shared file not found", session.permissions().download ? "file_missing" : "download_disabled");
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
    server.Put(R"(/api/upload/([0-9a-f]{32}))", [&session, auth, transfers](const Request& request, Response& response,
                                                               const httplib::ContentReader& read) {
        const auto size = number(request.get_header_value("Content-Length"));
        if (!size || *size > max_file_size || request.has_header("Content-Encoding") ||
            request.has_header("Transfer-Encoding")) {
            return fail(response, 400, "Invalid upload size or encoding");
        }
        const auto token = auth(request);
        const auto id = request.matches[1].str();
        const auto offset = number(request.get_header_value("X-Upload-Offset"));
        if (request.has_header("X-Upload-Offset") && (!offset || *offset > max_file_size || *size > 4 * 1024 * 1024)) {
            return fail(response, 400, "Invalid chunk offset or size");
        }
        if (!session.upload_allowed(token)) return fail(response, 409, "Upload approval required", "upload_unavailable");
        const auto slot = transfers->acquire();
        if (!slot) {
            response.set_header("Retry-After", "1");
            return fail(response, 503, "File transfer slots are busy", "transfer_busy");
        }
        UploadFailure failure{};
        const auto file = session.begin_upload(token, id, *size, offset, &failure);
        if (!file) {
            if (failure == UploadFailure::insufficient_space) return fail(response, 507, "Not enough space", "disk_space");
            if (failure == UploadFailure::storage) return fail(response, 507, "Receive folder unavailable", "storage_unavailable");
            return fail(response, 409, "Upload approval or matching offset required", "upload_unavailable");
        }
        UploadCleanup cleanup{session, token, id};
        const auto mode = std::ios::binary | std::ios::out |
            (offset.value_or(0) ? std::ios::in : std::ios::trunc);
        std::fstream output(*file->temporary, mode);
        if (!output) return fail(response, 507, "Unable to open receive file", "storage_unavailable");
        output.seekp(static_cast<std::streamoff>(offset.value_or(0)));

        std::uint64_t written = 0;
        const auto deadline = Session::Clock::now() + (offset ? std::chrono::seconds(40) : std::chrono::seconds(300));
        if (!read([&](const char* data, std::size_t length) {
            if (length > *size - written || Session::Clock::now() >= deadline || !session.uploading(token, id)) return false;
            output.write(data, static_cast<std::streamsize>(length));
            written += length;
            return static_cast<bool>(output);
        }) || written != *size) {
            if (!output) return fail(response, 507, "Cannot write received file", "storage_unavailable");
            return fail(response, 400, "Upload interrupted", "upload_interrupted");
        }
        output.close();
        const auto next = offset ? std::optional<std::uint64_t>(*offset + written) : std::nullopt;
        if (!output || !session.finish_upload(token, id, *file, next)) {
            return fail(response, 409, "Upload could not be saved");
        }
        cleanup.committed = true;
        response.status = 201;
        response.set_content(next ? "{\"transferred\":" + std::to_string(*next) + "}" : "{\"state\":\"complete\"}", json);
    });
}

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
                                    "connect-src 'self'; img-src 'self'; media-src 'self' blob:; base-uri 'none'; "
                                    "frame-ancestors 'none'; form-action 'self'"}
    });
    const auto auth_cookie = [this] { return "ts_" + std::to_string(port_); };
    const auto auth = [auth_cookie](const Request& request) {
        return cookie_token(request, auth_cookie());
    };

    server.set_pre_request_handler([this](const Request& request, Response& response) {
        using Result = httplib::Server::HandlerResponse;
        const bool loopback = local_socket(request);
        const auto interfaces = networks();
        if (!loopback && !permits_private_lan_peer(interfaces, request.local_addr, request.remote_addr)) {
            fail(response, 403, "Private LAN address required");
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
        if (!upload) {
            std::size_t limit = 0;
            if (request.method == "POST" && path == "/api/text") limit = max_text_size;
            if (request.method == "POST" && (path == "/api/cast/offer" || path == "/api/cast/answer")) {
                limit = max_sdp_size;
            }
            const auto length = number(request.get_header_value("Content-Length"));
            if (request.has_header("Transfer-Encoding") || request.has_header("Content-Encoding") ||
                (request.has_header("Content-Length") && (!length || *length > limit))) {
                fail(response, 400, "Unexpected request body");
                return Result::Handled;
            }
        }
        return Result::Unhandled;
    });

    register_pages(server);
    register_pairing(server, session, secure_, auth_cookie, auth);
    register_cast(server, session, auth, [this] { return networks(); });
    register_transfers(server, session, auth);
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
