#include "core/http_server.hpp"

#include "core/session.hpp"
#include "core/peer_link.hpp"
#include "core/folder_archive.hpp"
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
    return request.local_addr == "127.0.0.1" && request.remote_addr == "127.0.0.1" &&
        (!request.has_header("X-LAN-Source") || request.get_header_value("X-LAN-Source") == "127.0.0.1");
}

std::string remote_address(const Request& request) {
    return request.remote_addr == "127.0.0.1" && request.has_header("X-LAN-Source") ?
        request.get_header_value("X-LAN-Source") : request.remote_addr;
}
std::string local_address(const Request& request) {
    return request.remote_addr == "127.0.0.1" && request.has_header("X-LAN-Local") ?
        request.get_header_value("X-LAN-Local") : request.local_addr;
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
        ",\"ready\":" + boolean(signal.ready) + ",\"frames\":" + boolean(signal.frames) + "}";
}

// mDNS host candidates remain opaque to the service. A LAN may route multiple private subnets.
bool mdns_address(std::string_view address) {
    return address.size() > 6 && address.substr(address.size() - 6) == ".local" &&
        address.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") == std::string_view::npos;
}

bool lan_address(const std::string& address, std::string_view expected_peer) {
    if (mdns_address(address)) return true;
    return is_private_ipv4(address) && (expected_peer.empty() || expected_peer == address);
}

std::string lan_sdp(const std::string& body, const std::string& private_endpoint = {}, bool restrict_numeric = false) {
    if (body.rfind("v=0", 0) != 0 || body.find('\0') != std::string::npos) return {};
    std::istringstream stream(body);
    std::string line;
    std::string result;
    bool candidate = false;
    unsigned aliases = 0;
    const bool can_alias = is_private_ipv4(private_endpoint);
    const std::string_view expected_peer = restrict_numeric ? private_endpoint : std::string_view{};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("a=candidate:", 0) == 0) {
            std::istringstream fields(line);
            std::string foundation, component, protocol, priority, address, port, type, host;
            if (!(fields >> foundation >> component >> protocol >> priority >> address >> port >> type >> host) ||
                type != "typ" || host != "host" || !lan_address(address, expected_peer)) continue;
            candidate = true;
            // The TCP endpoint provides a fallback when the browsers cannot resolve each other's mDNS names.
            if (can_alias && mdns_address(address) && (protocol == "udp" || protocol == "UDP") && component == "1" &&
                aliases < 24) {
                const auto candidate_port = number(port);
                if (candidate_port && *candidate_port && *candidate_port <= 65535) {
                    result += "a=candidate:ts" + std::to_string(++aliases) + " 1 udp 2122260222 " +
                        private_endpoint + ' ' + std::to_string(*candidate_port) + " typ host\r\n";
                }
            }
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

std::string_view preview_type(const File& file) {
    auto extension = file.path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".mp4") return "video/mp4";
    if (extension == ".webm") return "video/webm";
    if (extension == ".pdf") return "application/pdf";
    if (file.size <= 2 * 1024 * 1024 && (extension == ".txt" || extension == ".log" ||
        extension == ".csv" || extension == ".md" || extension == ".json")) return "text/plain; charset=utf-8";
    return {};
}

void send_file(Session& session, const std::string& token, const File& file, Response& response,
               const std::shared_ptr<TransferSlots>& slots, const std::string& folder = {}, bool preview = false,
               bool attachment = false) {
    if (!file.source) {
        return fail(response, 409, "File is unavailable", "file_unavailable");
    }
    const auto slot = slots->acquire();
    if (!slot) {
        response.set_header("Retry-After", "1");
        return fail(response, 503, "File transfer slots are busy", "transfer_busy");
    }
    if (!folder.empty()) {
        std::error_code error;
        if (std::filesystem::canonical(file.path, error) != file.path || error) {
            return fail(response, 409, "File changed or is unavailable", "file_changed");
        }
    }
    auto input = std::make_shared<SharedSource>(file.path);
    if (!(input->version() == file.source)) {
        return fail(response, 409, "File changed or is unavailable", "file_changed");
    }
    const auto mime = preview ? preview_type(file) :
        file.temporary_directory ? "application/zip" : "application/octet-stream";
    if (mime.empty()) return fail(response, 415, "File preview is unavailable", "preview_unavailable");
    if (preview) response.set_header("Content-Security-Policy", "sandbox; default-src 'none'");
    response.set_header("Content-Disposition", std::string(preview ? "inline" : "attachment") + "; filename*=UTF-8''" +
                        httplib::encode_uri_component(file.name));
    const auto activity = random_token();
    response.set_content_provider(static_cast<std::size_t>(file.size), std::string(mime),
        [&session, directory = file.temporary_directory, input, id = file.id, folder, version = file.source, attachment,
         token, activity, name = file.name, size = file.size](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            session.download_progress(activity, name, size, offset);
            if (attachment ? !session.message_attachment(token, id) : !session.downloading(token, id, folder)) return false;
            std::array<char, 64 * 1024> chunk{};
            const auto count = std::min(length, chunk.size());
            return input->read(offset, chunk.data(), count) && input->version() == version &&
                sink.write(chunk.data(), count);
        }, [slot, &session, activity](bool success) { session.download_finished(activity, success); });
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
    server.Get("/icon.png", [](const Request&, Response& response) {
        response.set_content(reinterpret_cast<const char*>(web::icon), sizeof(web::icon), "image/png");
    });
    server.Get("/", [](const Request&, Response& response) { response.set_content(web::index, "text/html; charset=utf-8"); });
    server.Get("/styles.css", [](const Request&, Response& response) { response.set_content(web::styles, "text/css; charset=utf-8"); });
    server.Get("/app.js", [](const Request&, Response& response) { response.set_content(web::script, "text/javascript; charset=utf-8"); });
    server.Get("/appearance.js", [](const Request&, Response& response) { response.set_content(web::appearance, "text/javascript; charset=utf-8"); });
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
        if (!pairing) {
            const auto state = session.snapshot(SnapshotPage::home);
            return fail(response, state.pairing ? 409 : 403, "Invitation unavailable",
                        state.pairing ? "device_busy" : state.active ? "invalid_invitation" : "session_expired");
        }
        response.set_header("Set-Cookie", auth_cookie() + "=" + pairing->token +
            "; Path=/; HttpOnly; SameSite=Strict" + (secure ? "; Secure" : ""));
        response.status = 201;
        response.set_content("{\"verification\":" + json_string(pairing->verification) + "}", json);
    });
    server.Post("/api/pair/trusted", [secure, &session, auth_cookie](const Request& request, Response& response,
                                                                   const httplib::ContentReader&) {
        if (!secure) return fail(response, 403, "HTTPS is required for remembered devices");
        auto trusted = cookie_token(request, auth_cookie() + "_trusted");
        if (trusted.empty()) trusted = cookie_token(request, trust_cookie);
        const auto pairing = session.reconnect(trusted);
        if (!pairing) return fail(response, 403, "Remembered device or active session unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=" + pairing->token + "; Path=/; HttpOnly; Secure; SameSite=Strict");
        response.status = 201;
    });
    server.Get("/api/pair", [secure, &session, auth, auth_cookie](const Request& request, Response& response) {
        const auto pairing = session.pairing(auth(request));
        if (!pairing) return fail(response, 401, "Session is unavailable", session.pairing_error(auth(request)));
        session.touch(pairing->token);
        const bool trusted = secure && !pairing->trusted_key.empty();
        const auto trusted_cookie = std::string(trust_cookie);
        if (trusted && cookie_token(request, trusted_cookie) != pairing->trusted_key) {
            response.set_header("Set-Cookie", trusted_cookie + "=" + pairing->trusted_key +
                "; Path=/; Max-Age=31536000; HttpOnly; Secure; SameSite=Strict");
        }
        const auto permissions = session.permissions();
        response.set_content("{\"state\":" + json_string(state_name(pairing->state)) +
            ",\"computer\":" + json_string(device_hostname()) +
            ",\"remainingSeconds\":" + std::to_string(pairing->remaining_seconds) +
            ",\"unlimited\":" + boolean(session.snapshot(SnapshotPage::home).unlimited) +
            ",\"verification\":" + json_string(pairing->verification) + ",\"download\":" + boolean(permissions.download) +
            ",\"upload\":" + boolean(permissions.upload) + ",\"text\":" + boolean(permissions.text) +
            ",\"castAllowed\":" + boolean(permissions.cast) +
            ",\"trusted\":" + boolean(trusted) + ",\"textRevision\":" +
            std::to_string(session.text_revision()) + ",\"castReady\":" +
            boolean(session.capture_ready(pairing->token).value_or(false)) + ",\"filesRevision\":" +
            std::to_string(session.files_revision()) + ",\"offersRevision\":" +
            std::to_string(session.offers_revision()) + "}", json);
    });

    server.Post("/api/pair/extend", [&session, auth](const Request& request, Response& response) {
        if (!session.authorized(auth(request))) return fail(response, 403, "Connection approval required");
        response.status = session.extend() ? 204 : 409;
    });
    server.Delete("/api/pair", [secure, &session, auth, auth_cookie](const Request& request, Response& response,
                                                            const httplib::ContentReader&) {
        if (!session.disconnect(auth(request))) return fail(response, 401, "Session is unavailable");
        response.set_header("Set-Cookie", auth_cookie() + "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict" +
            (secure ? "; Secure" : ""));
        response.status = 204;
    });

    server.Delete("/api/text", [&session, auth](const Request& request, Response& response, const httplib::ContentReader&) {
        if (!session.clear_messages(auth(request))) return fail(response, 403, "Text sharing is unavailable");
        response.status = 204;
    });
    server.Get("/api/text", [&session, auth](const Request& request, Response& response) {
        const auto messages = session.messages(auth(request));
        if (!messages) return fail(response, 403, "Text sharing is unavailable");
        std::string body = "[";
        for (const auto& message : *messages) {
            if (body.size() > 1) body += ',';
            const auto attachment = session.message_attachment(auth(request), message.attachment);
            const auto available = attachment ? message.attachment : std::string{};
            const auto mime = attachment ? preview_type(*attachment) : std::string_view{};
            body += "{\"text\":" + json_string(message.text) + ",\"desktop\":" + boolean(message.from_desktop) +
                ",\"sentAt\":" + std::to_string(message.sent_at) + ",\"attachment\":" + json_string(available) +
                ",\"attachmentType\":" + json_string(mime) + '}';
        }
        response.set_content(body + ']', json);
    });
    server.Post("/api/text", [&session, auth](const Request& request, Response& response) {
        if (!session.authorized(auth(request))) return fail(response, 403, "Connection approval required");
        if (!session.send_text(auth(request), request.body)) return fail(response, 400, "Invalid or disabled text");
        response.status = 201;
    });
}

void register_cast(httplib::Server& server, Session& session, const Authorize& auth) {
    const auto host_key = [](const Request& request) {
        return local_socket(request) ? request.get_header_value("X-Cast-Key") : std::string{};
    };
    server.Get("/api/cast/host", [&session, host_key](const Request& request, Response& response) {
        const auto signal = session.cast_host(host_key(request));
        if (!signal) return fail(response, 403, "Desktop cast authorization required");
        const auto state = session.snapshot(SnapshotPage::home);
        auto body = cast_json(*signal);
        body.pop_back();
        body += ",\"target\":" + json_string(state.pairing ? state.pairing->name : "未连接设备") + '}';
        response.set_content(body, json);
    });
    server.Post("/api/cast/ready", [&session, host_key](const Request& request, Response& response,
                                                       const httplib::ContentReader&) {
        if (!session.cast_ready(host_key(request))) return fail(response, 403, "Desktop cast authorization required");
        response.status = 204;
    });
    server.Post("/api/cast/offer", [&session, host_key](const Request& request, Response& response) {
        const auto key = host_key(request);
        if (!session.cast_host(key)) return fail(response, 403, "Desktop cast authorization required");
        auto offer = lan_sdp(request.body);
        if (offer.empty()) return fail(response, 409, "No private ICE candidate", "no_lan_candidate");
        if (!session.cast_offer(key, std::move(offer))) return fail(response, 409, "No LAN viewer");
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
        auto signal = session.cast_viewer(token, request.get_header_value("X-Cast-Watch") == "1",
                                               request.get_header_value("X-Cast-Viewer"),
                                               request.get_header_value("X-Cast-Frames") == "1");
        if (!signal) return fail(response, 409, "Another viewer is active", "viewer_busy");
        if (!signal->offer.empty() && !local_socket(request)) signal->offer = lan_sdp(signal->offer, local_address(request));
        response.set_content(cast_json(*signal), json);
    });
    server.Post("/api/cast/answer", [&session, auth](const Request& request, Response& response) {
        const auto token = auth(request);
        if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
        const auto revision = number(request.get_header_value("X-Cast-Revision"));
        auto answer = local_socket(request) ? lan_sdp(request.body) : lan_sdp(request.body, remote_address(request), true);
        if (answer.empty()) return fail(response, 409, "No private ICE candidate", "no_lan_candidate");
        if (!revision || *revision > std::numeric_limits<unsigned>::max() ||
            !session.cast_answer(token, static_cast<unsigned>(*revision), std::move(answer),
                                 request.get_header_value("X-Cast-Viewer"))) {
            return fail(response, 409, "Cast offer changed or no LAN candidate is available");
        }
        response.status = 201;
    });
    server.Post("/api/cast/frame", [&session, host_key](const Request& request, Response& response) {
        const auto key = host_key(request);
        if (key.empty()) return fail(response, 403, "Desktop cast authorization required");
        const auto revision = number(request.get_header_value("X-Cast-Revision"));
        const auto& jpeg = request.body;
        if (request.get_header_value("Content-Type") != "image/jpeg" || jpeg.size() < 4 ||
            jpeg.compare(0, 2, "\xff\xd8") != 0 || jpeg.compare(jpeg.size() - 2, 2, "\xff\xd9") != 0) {
            return fail(response, 400, "Expected a JPEG frame");
        }
        if (!revision || *revision > std::numeric_limits<unsigned>::max() ||
            !session.publish_cast_frame(key, static_cast<unsigned>(*revision), jpeg)) {
            return fail(response, 409, "Frame viewer is unavailable");
        }
        response.status = 204;
    });
    const auto frame_wait = std::make_shared<std::mutex>();
    server.Get("/api/cast/frame", [&session, auth, frame_wait](const Request& request, Response& response) {
        const auto after = number(request.get_header_value("X-Cast-After")).value_or(0);
        std::unique_lock wait(*frame_wait, std::defer_lock);
        const bool wait_for_frame = request.get_header_value("X-Cast-Wait") == "1" && wait.try_lock();
        auto frame = session.cast_frame(auth(request), request.get_header_value("X-Cast-Viewer"), after,
                                        std::chrono::milliseconds(wait_for_frame ? 800 : 0));
        if (!frame) return fail(response, 403, "Active frame viewer required");
        if (frame->jpeg.empty()) {
            response.status = 204;
            return;
        }
        response.set_header("X-Cast-Sequence", std::to_string(frame->sequence));
        response.set_content(std::move(frame->jpeg), "image/jpeg");
    });
}

std::string files_json(const std::vector<File>& files, bool include_folders) {
    std::string body = "[";
    for (const auto& file : files) {
        if (file.folder && !include_folders) continue;
        if (body.size() > 1) body += ',';
        body += "{\"id\":" + json_string(file.id) + ",\"name\":" + json_string(file.name) +
                ",\"size\":" + std::to_string(file.size);
        if (file.folder) body += ",\"folder\":true";
        else if (!file.source) body += ",\"unavailable\":true";
        else if (const auto mime = preview_type(file); !mime.empty()) body += ",\"preview\":" + json_string(mime);
        body += '}';
    }
    return body + ']';
}

void register_transfers(httplib::Server& server, Session& session, const Authorize& auth) {
    const auto transfers = std::make_shared<TransferSlots>();
    const auto archive_mutex = std::make_shared<std::mutex>();
    server.Get(R"(/api/text/attachment/([0-9a-f]{32}))", [&session, auth, transfers](const Request& request, Response& response) {
        const auto token = auth(request);
        const auto file = session.message_attachment(token, request.matches[1].str());
        if (!file) return fail(response, 404, "Attachment is unavailable");
        send_file(session, token, *file, response, transfers, {}, request.get_param_value("preview") == "1", true);
    });
    server.Get("/api/files", [&session, auth](const Request& request, Response& response) {
        const auto files = session.shared(auth(request));
        if (!files) return fail(response, 403, "Connection approval required");
        response.set_content(files_json(*files, request.get_param_value("folders") == "1"), json);
    });
    server.Get(R"(/api/download/([0-9a-f]{32}))", [&session, auth, transfers](const Request& request, Response& response) {
        const auto token = auth(request);
        const auto files = session.shared(token);
        if (!files) return fail(response, 403, "Connection approval required");
        for (const auto& file : *files) {
            if (!file.folder && file.id == request.matches[1].str()) {
                return send_file(session, token, file, response, transfers, {}, request.get_param_value("preview") == "1");
            }
        }
        fail(response, 404, "Shared file not found", session.permissions().download ? "file_missing" : "download_disabled");
    });
    server.Get(R"(/api/folders/([0-9a-f]{32})/entries/([0-9a-f]{32}))",
        [&session, auth](const Request& request, Response& response) {
            const auto token = auth(request);
            if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
            auto entries = session.folder_contents(token, request.matches[1].str(), request.matches[2].str());
            if (!entries) return fail(response, 404, "Shared directory is unavailable", "folder_unavailable");
            if (request.has_param("offset")) {
                const auto offset = number(request.get_param_value("offset"));
                auto query = request.get_param_value("q");
                if (!offset || *offset > 10000 || query.size() > 400) return fail(response, 400, "Invalid directory page");
                const auto lower = [](std::string value) {
                    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return value;
                };
                if (!query.empty()) {
                    query = lower(std::move(query));
                    entries->erase(std::remove_if(entries->begin(), entries->end(), [&](const File& file) {
                        return lower(file.name).find(query) == std::string::npos;
                    }), entries->end());
                }
                if (request.get_param_value("sort") == "size") {
                    std::stable_sort(entries->begin(), entries->end(), [](const File& a, const File& b) {
                        return a.folder == b.folder ? a.size > b.size : a.folder;
                    });
                }
                constexpr std::size_t page = 80;
                const auto first = std::min(static_cast<std::size_t>(*offset), entries->size());
                const auto last = std::min(first + page, entries->size());
                response.set_header("X-Folder-Count", std::to_string(entries->size()));
                response.set_content(files_json({entries->begin() + first, entries->begin() + last}, true), json);
                return;
            }
            response.set_content(files_json(*entries, true), json);
        });
    server.Get(R"(/api/folders/([0-9a-f]{32})/download/([0-9a-f]{32}))",
        [&session, auth, transfers](const Request& request, Response& response) {
            const auto token = auth(request);
            if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
            const auto root = request.matches[1].str();
            const auto child = session.folder_file(token, root, request.matches[2].str());
            if (!child) return fail(response, 404, "Shared file is unavailable", "file_missing");
            send_file(session, token, *child, response, transfers, root, request.get_param_value("preview") == "1");
        });
    server.Get(R"(/api/folders/([0-9a-f]{32})/zip/([0-9a-f]{32}))",
        [&session, auth, transfers, archive_mutex](const Request& request, Response& response) {
            const auto token = auth(request);
            if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
            const auto root = request.matches[1].str();
            const auto directory = request.matches[2].str();
            std::vector<std::string> selected;
            const auto range = request.params.equal_range("file");
            for (auto entry = range.first; entry != range.second; ++entry) {
                if (selected.size() >= 80 || entry->second.size() != 32) return fail(response, 400, "Invalid selection");
                selected.push_back(entry->second);
            }
            std::unique_lock archive(*archive_mutex, std::try_to_lock);
            if (!archive.owns_lock()) {
                response.set_header("Retry-After", "2");
                return fail(response, 503, "Archive is busy", "transfer_busy");
            }
            const auto files = session.folder_selection(token, root, directory, selected);
            if (!files) return fail(response, 404, "Folder selection is unavailable");
            const auto& folder = files->front();
            const auto deadline = Session::Clock::now() + std::chrono::minutes(2);
            try {
                auto zip = archive_files(folder.name, folder.path, *files, [&] {
                    return request.is_connection_closed() || Session::Clock::now() >= deadline ||
                        !session.downloading(token, root);
                });
                const auto path = *zip.directory / "folder.zip";
                const auto source = SharedSource(path).version();
                if (!source || !session.downloading(token, root)) return fail(response, 409, "Archive is unavailable");
                const File download{root, std::move(zip.name), source->size, path, source, std::move(zip.directory)};
                send_file(session, token, download, response, transfers);
            } catch (const std::exception&) {
                fail(response, 409, "Unable to archive the selection", "archive_unavailable");
            }
        });

    server.Post("/api/offers", [&session, auth](const Request& request, Response& response,
                                              const httplib::ContentReader&) {
        const auto token = auth(request);
        if (!session.authorized(token)) return fail(response, 403, "Connection approval required");
        auto name = decode_filename(request.get_header_value("X-File-Name"));
        const auto size = number(request.get_header_value("X-File-Size"));
        if (!name || !size || *size > max_file_size) return fail(response, 400, "Invalid file name or size");
        const auto encoded_directory = request.get_header_value("X-Upload-Directory");
        if (encoded_directory.size() > 3 * 1024) return fail(response, 400, "Upload directory is too long");
        const auto directory = httplib::decode_uri_component(encoded_directory);
        const auto offer = session.offer(token, std::move(*name), *size, request.get_header_value("X-Upload-Batch"),
                                         directory, request.get_header_value("X-Upload-Image") == "1");
        if (!offer) return fail(response, 409, "Unable to create a new request");
        response.status = 201;
        response.set_header("X-Offer-ID", offer->id);
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
        if (!read([&](const char* data, std::size_t length) {
            if (length > *size - written || !session.uploading(token, id)) return false;
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

HttpServer::HttpServer(Session& session, const std::string& certificate, const std::string& key,
                       std::function<void()> changed)
    : session_(session), secure_(!certificate.empty() || !key.empty()), gateway_key_(random_token()) {
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
    peer_ = std::make_unique<PeerLink>(session, std::move(changed));
    // Four transfer slots are reserved by register_transfers. Keep additional workers
    // available for pairing, status, approval and cast control requests.
    server.new_task_queue = [] { return new httplib::ThreadPool(12, 12, 48); };
    server.set_payload_max_length(max_file_size);
    server.set_keep_alive_max_count(4);
    server.set_read_timeout(10);
    server.set_write_timeout(10);
    server.set_default_headers({
        {"Cache-Control", "no-store"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Content-Type-Options", "nosniff"},
        {"Content-Security-Policy", "default-src 'none'; script-src 'self'; worker-src 'self' blob:; style-src 'self'; "
                                    "connect-src 'self'; img-src 'self' blob:; media-src 'self' blob:; base-uri 'none'; "
                                    "frame-ancestors 'none'; form-action 'self'"}
    });
    const auto auth_cookie = [this] { return "ts_" + std::to_string(port_); };
    const auto auth = [auth_cookie](const Request& request) {
        if (request.has_header("X-Desktop-Key")) return request.get_header_value("X-Desktop-Key");
        return cookie_token(request, auth_cookie());
    };

    server.set_pre_request_handler([this](const Request& request, Response& response) {
        using Result = httplib::Server::HandlerResponse;
        const bool loopback = local_socket(request);
        const bool gateway = request.local_addr == "127.0.0.1" && request.remote_addr == "127.0.0.1" &&
            request.get_header_value("X-Gateway-Key") == gateway_key_;
        const auto interfaces = networks();
        if (!gateway && !loopback && !permits_private_lan_peer(interfaces, request.local_addr, request.remote_addr)) {
            fail(response, 403, "Private LAN address required");
            return Result::Handled;
        }
        const auto header = request.get_header_value("Host");
        const auto host = header.substr(0, header.find(':'));
        if (host != request.local_addr && !(loopback && host == "localhost")) {
            fail(response, 400, "Open the IPv4 address shown in TempShare");
            return Result::Handled;
        }
        if (request.method == "GET" && request.path == "/" && session_.snapshot(SnapshotPage::home).pairing &&
            !session_.pairing(cookie_token(request, "ts_" + std::to_string(port_)))) {
            const auto route = available_session();
            if (!route.empty()) {
                response.set_redirect(route + "/");
                return Result::Handled;
            }
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
            if (path.substr(0, 3) == "/s/") limit = 4 * 1024 * 1024;
            if (path.substr(0, 10) == "/peer/api/") limit = 4 * 1024 * 1024;
            if (request.method == "POST" && path == "/api/text") limit = max_text_size;
            if (request.method == "POST" && (path == "/api/cast/offer" || path == "/api/cast/answer")) {
                limit = max_sdp_size;
            }
            if (request.method == "POST" && path == "/api/cast/frame") {
                if (!loopback) {
                    fail(response, 403, "Desktop cast authorization required");
                    return Result::Handled;
                }
                limit = max_cast_frame_size;
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
    const auto gateway_handler = [this](const Request& request, Response& response) { forward_session(request, response); };
    server.Get(R"(/s/([1-4])(/.*)?)", gateway_handler);
    server.Post(R"(/s/([1-4])(/.*)?)", gateway_handler);
    server.Put(R"(/s/([1-4])(/.*)?)", gateway_handler);
    server.Delete(R"(/s/([1-4])(/.*)?)", gateway_handler);
    server.Get("/peer", [](const Request& request, Response& response) {
        if (!local_socket(request)) return fail(response, 403, "Local desktop authorization required");
        response.set_content(web::index, "text/html; charset=utf-8");
    });
    peer_->register_routes(server);
    register_pairing(server, session, secure_, auth_cookie, auth);
    register_cast(server, session, auth);
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
    if (server_->is_running()) peer_->start(port_, secure_, loopback);
    return server_->is_running();
}

void HttpServer::stop() {
    if (peer_) peer_->stop();
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

PeerLink& HttpServer::peer() { return *peer_; }
const PeerLink& HttpServer::peer() const { return *peer_; }

std::string HttpServer::url(const std::string& address) const {
    std::lock_guard lock(networks_mutex_);
    return std::string(secure_ ? "https://" : "http://") + address + ':' +
        std::to_string(public_port_ ? public_port_ : port_) + public_route_ + '/';
}

bool HttpServer::probe(const std::string& address) const {
    auto origin = url(address);
    origin.pop_back();
    httplib::Client client(origin);
    client.set_connection_timeout(0, 350000);
    client.set_read_timeout(0, 350000);
    client.set_write_timeout(0, 350000);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // This request checks the listener on the machine's own interface.
    client.enable_server_certificate_verification(false);
#endif
    const auto response = client.Get("/api/health");
    return response && response->status == 200;
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


void HttpServer::configure_gateway(const std::vector<HttpServer*>& sessions) {
    {
        std::lock_guard lock(networks_mutex_);
        routes_ = sessions;
    }
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        auto* target = sessions[index];
        const auto route = index ? "/s/" + std::to_string(index + 1) : std::string{};
        {
            std::lock_guard lock(target->networks_mutex_);
            target->public_port_ = port_;
            target->public_route_ = route;
        }
        target->peer().set_public_endpoint(port_, route);
        target->peer().set_admission([this] { return available_session(); });
    }
}

std::string HttpServer::available_session() {
    std::vector<HttpServer*> routes;
    { std::lock_guard lock(networks_mutex_); routes = routes_; }
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto state = routes[index]->session_.snapshot(SnapshotPage::home);
        if (!state.pairing && !routes[index]->peer().snapshot().attached) {
            if (!state.active) routes[index]->session_.start();
            return "/s/" + std::to_string(index + 1);
        }
    }
    return {};
}

void HttpServer::forward_session(const Request& request, Response& response) {
    const auto index = static_cast<std::size_t>(request.matches[1].str()[0] - '1');
    HttpServer* target = nullptr;
    { std::lock_guard lock(networks_mutex_); if (index < routes_.size()) target = routes_[index]; }
    if (!target) return fail(response, 404, "Session route unavailable");
    std::string path = request.target.substr(4);
    if (path.empty()) path = "/";
    if (path.front() != '/') return fail(response, 400, "Invalid session route");
    const auto origin = std::string(target->secure_ ? "https://" : "http://") + "127.0.0.1:" + std::to_string(target->port_);
    auto headers = request.headers;
    headers.erase("Host"); headers.erase("Content-Length"); headers.erase("Transfer-Encoding");
    headers.erase("X-Gateway-Key"); headers.erase("X-LAN-Source"); headers.erase("X-LAN-Local");
    headers.emplace("X-Gateway-Key", target->gateway_key_);
    headers.emplace("X-LAN-Source", request.remote_addr);
    headers.emplace("X-LAN-Local", request.local_addr);
    auto prepare = [origin] {
        auto remote = std::make_unique<httplib::Client>(origin);
        remote->set_connection_timeout(2); remote->set_read_timeout(10); remote->set_write_timeout(10);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        remote->enable_server_certificate_verification(false); // 仅连接本进程的回环服务。
#endif
        return remote;
    };
    auto remote = prepare();
    const bool stream = request.method == "GET" && (path.find("/download/") != std::string::npos ||
        path.find("/attachment/") != std::string::npos || path.find("/zip/") != std::string::npos);
    if (stream) {
        const auto head = remote->Head(path, headers);
        if (!head || head->status >= 400) return fail(response, head ? head->status : 502, "Download unavailable");
        response.status = head->status;
        for (const auto* name : {"Content-Disposition", "Content-Type", "Content-Range", "Accept-Ranges"})
            if (head->has_header(name)) response.set_header(name, head->get_header_value(name));
        response.set_chunked_content_provider(head->get_header_value("Content-Type"),
            [prepare, path, headers](std::size_t, httplib::DataSink& sink) {
                auto input = prepare();
                const auto result = input->Get(path, headers,
                    [](const Response& item) { return item.status >= 200 && item.status < 300; },
                    [&](const char* data, std::size_t size) { return sink.write(data, size); });
                if (!result) return false;
                sink.done(); return true;
            });
        return;
    }
    Request outbound;
    outbound.method = request.method; outbound.path = path; outbound.headers = headers; outbound.body = request.body;
    std::string body;
    outbound.content_receiver = [&](const char* data, std::size_t size, std::uint64_t, std::uint64_t) {
        if (size > 4 * 1024 * 1024 - body.size()) return false;
        body.append(data, size); return true;
    };
    const auto result = remote->send(outbound);
    if (!result) return fail(response, 502, "Session unavailable");
    response.status = result->status;
    response.set_content(body, result->get_header_value("Content-Type"));
    for (const auto* name : {"Set-Cookie", "Location", "X-Desktop-Route", "X-Offer-ID", "Content-Disposition",
                             "Content-Range", "Accept-Ranges", "Retry-After", "X-Cast-Sequence"})
        for (std::size_t i = 0; i < result->get_header_value_count(name); ++i)
            response.set_header(name, result->get_header_value(name, "", i));
    if (request.method == "HEAD" && result->has_header("Content-Length"))
        response.set_header("Content-Length", result->get_header_value("Content-Length"));
}

}  // namespace tempshare
