#include "core/peer_link.hpp"
#include "core/discovery.hpp"
#include "core/peer_wire.hpp"
#include <httplib.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <utility>

namespace tempshare {
namespace {
using namespace std::chrono_literals;
using Fields = std::vector<std::string>;
httplib::Headers credentials(const std::string& token) {
    return {{"X-TempShare", "1"}, {"X-Desktop-Key", token}};
}
std::unique_ptr<httplib::Client> client(const LanEndpoint& endpoint) {
    auto result = std::make_unique<httplib::Client>(endpoint.origin());
    result->set_connection_timeout(2);
    result->set_read_timeout(5);
    result->set_write_timeout(10);
    result->set_follow_location(false);
    return result;
}
std::optional<Fields> get_state(httplib::Client& remote, const std::string& route,
                               const std::string& token, int& status) {
    std::string body;
    const auto result = remote.Get(route + "/api/desktop/state", credentials(token),
        [&](const char* data, std::size_t size) {
            if (size > peer_wire::limit - body.size()) return false;
            body.append(data, size); return true;
        });
    status = result ? result->status : 0;
    return result && result->status == 200 ? peer_wire::decode(body) : std::nullopt;
}
std::string cookie(const httplib::Request& request, const std::string& key) {
    const auto text = request.get_header_value("Cookie");
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find(';', start);
        auto part = text.substr(start, end - start);
        const auto first = part.find_first_not_of(" ");
        if (first != std::string::npos) part.erase(0, first);
        if (part.rfind(key + "=", 0) == 0) return part.substr(key.size() + 1);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}
bool loopback(const httplib::Request& request) {
    return request.local_addr == "127.0.0.1" && request.remote_addr == "127.0.0.1" &&
        (!request.has_header("X-LAN-Source") || request.get_header_value("X-LAN-Source") == "127.0.0.1");
}
void failure(httplib::Response& response, int status, const std::string& code) {
    response.status = status;
    response.set_content("{\"code\":" + json_string(code) + "}", "application/json");
}
}

std::string LanEndpoint::origin() const {
    return std::string(secure ? "https://" : "http://") + host + ":" + std::to_string(port);
}

std::optional<LanEndpoint> parse_lan_endpoint(std::string value, bool allow_loopback) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.find_first_of("@\\? \r\n\t") != std::string::npos) return {};
    if (value.find("://") == std::string::npos) value = "http://" + value;
    LanEndpoint endpoint;
    if (value.rfind("https://", 0) == 0) { endpoint.secure = true; value.erase(0, 8); }
    else if (value.rfind("http://", 0) == 0) value.erase(0, 7);
    else return {};
    const auto hash = value.find('#');
    if (hash != std::string::npos) {
        endpoint.invitation = value.substr(hash + 1);
        if (!peer_wire::token(endpoint.invitation) &&
            !(endpoint.invitation.size() == 6 && endpoint.invitation.find_first_not_of("0123456789") == std::string::npos)) return {};
        value.resize(hash);
    }
    const auto slash = value.find('/');
    if (slash != std::string::npos) {
        endpoint.route = value.substr(slash);
        value.resize(slash);
        while (!endpoint.route.empty() && endpoint.route.back() == '/') endpoint.route.pop_back();
        if (!endpoint.route.empty() && !(endpoint.route.size() == 4 && endpoint.route.rfind("/s/", 0) == 0 &&
            endpoint.route[3] >= '1' && endpoint.route[3] <= '4')) return {};
    }
    const auto colon = value.find(':');
    endpoint.host = value.substr(0, colon);
    if (!is_private_ipv4(endpoint.host) && !(allow_loopback && endpoint.host == "127.0.0.1")) {
        if (endpoint.host.empty() || endpoint.host.size() > 253 ||
            endpoint.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") != std::string::npos ||
            endpoint.host.find_first_not_of("0123456789.") == std::string::npos) return {};
    }
    if (colon != std::string::npos) {
        const auto port = peer_wire::number(value.substr(colon + 1));
        if (!port || !*port || *port > 65535) return {};
        endpoint.port = static_cast<unsigned short>(*port);
    }
    return endpoint;
}

PeerLink::PeerLink(Session& session, std::function<void()> changed)
    : session_(session), changed_(std::move(changed)), browser_key_(random_token()), device_id_(random_token()) {}
PeerLink::~PeerLink() { stop(); }
void PeerLink::start(unsigned short port, bool secure, bool allow_loopback) {
    std::lock_guard lock(mutex_);
    port_ = port; secure_ = secure; loopback_ = allow_loopback;
    worker_ = std::thread(&PeerLink::run, this);
}
void PeerLink::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cancel_ = true;
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (transfer_.joinable()) transfer_.join();
}
void PeerLink::set_public_endpoint(unsigned short port, std::string route) {
    std::lock_guard lock(mutex_); port_ = port; public_route_ = std::move(route);
}
void PeerLink::set_admission(std::function<std::string()> admission) {
    std::lock_guard lock(mutex_); available_route_ = std::move(admission);
}
void PeerLink::set_identity(std::string identity, std::string name) {
    std::lock_guard lock(mutex_); device_id_ = std::move(identity); device_name_ = std::move(name);
}
PeerLinkState PeerLink::snapshot() const { std::lock_guard lock(mutex_); return state_; }
std::string PeerLink::browser_key() const { std::lock_guard lock(mutex_); return browser_key_; }

bool PeerLink::join(const std::string& address) {
    const auto endpoint = parse_lan_endpoint(address, loopback_);
    if (!endpoint) return false;
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (endpoint->secure) return false;
#endif
    const auto local = session_.snapshot(SnapshotPage::home);
    if (local.pairing) return false;
    if (!local.active) session_.start();
    {
        std::lock_guard lock(mutex_);
        if (state_.attached || connecting_) return false;
        endpoint_ = *endpoint;
        cancel_ = true;
        const bool transferring = state_.transferring;
        state_ = {};
        state_.transferring = transferring;
        state_.outgoing = true;
        state_.device = endpoint->host;
        state_.address = endpoint->origin() + endpoint->route;
        state_.status = "正在请求连接…";
        connecting_ = true; last_message_ = 0; ++generation_;
        browser_key_ = random_token();
    }
    condition_.notify_all();
    if (changed_) changed_();
    return true;
}

void PeerLink::leave() {
    std::string token;
    { std::lock_guard lock(mutex_); token = local_token_; }
    if (!token.empty()) session_.disconnect(token);
    cancel_ = true;
    condition_.notify_all();
}

void PeerLink::connect() {
    LanEndpoint endpoint;
    unsigned short port;
    bool secure;
    std::uint64_t generation;
    std::string route, identity, name;
    { std::lock_guard lock(mutex_); endpoint = endpoint_; port = port_; secure = secure_; route = public_route_;
      identity = device_id_; name = device_name_.empty() ? device_hostname() : device_name_; generation = generation_; }
    if (!is_private_ipv4(endpoint.host) && endpoint.host != "127.0.0.1") {
        const auto addresses = resolve_lan_addresses(endpoint.host);
        if (addresses.empty()) {
            std::lock_guard lock(mutex_); connecting_ = false; state_.status = "主机名未解析到可用的私有IPv4，请粘贴对方地址"; return;
        }
        endpoint.host = addresses.front();
    }
    const auto local = session_.snapshot(SnapshotPage::home);
    const auto own = session_.pair(local.invitation, endpoint.host);
    if (!own) {
        std::lock_guard lock(mutex_); state_.status = "当前会话已有设备，请先离开或另选会话"; connecting_ = false;
        return;
    }
    auto remote = client(endpoint);
    auto headers = credentials(own->token);
    headers.emplace("X-Desktop-Port", std::to_string(port));
    headers.emplace("X-Desktop-Route", route);
    headers.emplace("X-Desktop-Secure", secure ? "1" : "0");
    headers.emplace("X-Desktop-ID", identity);
    headers.emplace("X-Device-Name", httplib::encode_uri_component(name));
    headers.emplace("X-Invitation", endpoint.invitation);
    std::string reply;
    const auto receive = [&](const char* data, std::size_t size) {
        if (size > 64 * 1024 - reply.size()) return false;
        reply.append(data, size); return true;
    };
    auto response = remote->Post(endpoint.route + "/api/desktop/pair", headers, "", "application/octet-stream", receive);
    if (response && response->status == 307) {
        const auto alternate = response->get_header_value("X-Desktop-Route");
        if (alternate.size() == 4 && alternate.rfind("/s/", 0) == 0 && alternate[3] >= '1' && alternate[3] <= '4') {
            endpoint.route = alternate;
            reply.clear();
            response = remote->Post(endpoint.route + "/api/desktop/pair", headers, "", "application/octet-stream", receive);
        }
    }
    const auto fields = response && response->status == 201 ? peer_wire::decode(reply) : std::nullopt;
    if (!fields || fields->size() != 4 || !peer_wire::token((*fields)[0]) || !peer_wire::token((*fields)[3]) ||
        !valid_filename((*fields)[2]) || (*fields)[2].size() > 80 || (*fields)[1].size() != 6 ||
        (*fields)[1].find_first_not_of("0123456789") != std::string::npos) {
        session_.disconnect(own->token);
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        state_.status = response && response->status == 409 ? "对方正在使用其他会话，请稍后重试" :
            response && response->status == 403 ? "邀请已失效，请重新获取连接码" : "无法连接，请检查对方地址、防火墙或版本";
        connecting_ = false; state_.attached = false;
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        if (!session_.pairing(own->token)) { connecting_ = false; state_.status = "连接请求已取消，请重新发起"; return; }
        endpoint_ = endpoint;
        local_token_ = own->token; remote_token_ = (*fields)[0];
        state_.device_id = (*fields)[3]; state_.device = (*fields)[2];
        state_.address = endpoint.origin() + endpoint.route;
        state_.verification = (*fields)[1];
        state_.attached = true; state_.online = true; connecting_ = false;
        state_.status = "等待对方确认 · 请核对验证码";
    }
}

void PeerLink::synchronize() {
    LanEndpoint endpoint;
    std::string local, token;
    std::uint64_t generation;
    bool outgoing;
    { std::lock_guard lock(mutex_); endpoint = endpoint_; local = local_token_; token = remote_token_;
      generation = generation_; outgoing = state_.outgoing; }
    if (!session_.pairing(local)) {
        auto remote = client(endpoint);
        remote->Delete(endpoint.route + "/api/pair", credentials(token));
        std::lock_guard lock(mutex_);
        if (generation != generation_) return;
        state_.attached = state_.approved = state_.online = false;
        state_.status = "已离开，文件和消息保留在本机会话"; local_token_.clear(); remote_token_.clear();
        return;
    }
    auto remote = client(endpoint);
    int status = 0;
    const auto fields = get_state(*remote, endpoint.route, token, status);
    if (!fields) {
        if (status == 401 || status == 403) {
            session_.disconnect(local);
            std::lock_guard lock(mutex_);
            if (generation != generation_) return;
            state_.attached = state_.approved = state_.online = false;
            state_.status = "对方已结束连接，可重新发起";
        } else {
            std::lock_guard lock(mutex_);
            if (generation != generation_) return;
            state_.online = false; state_.status = "连接暂时中断，正在重试";
        }
        return;
    }
    if (fields->size() < 12 || (*fields)[0] != "1" || !valid_filename((*fields)[1]) || (*fields)[1].size() > 80) return;
    const auto& f = *fields;
    const auto count = peer_wire::number(f[10]);
    if (!count || *count > 50) return;
    std::size_t position = 11;
    std::vector<Message> messages;
    for (std::uint64_t i = 0; i < *count; ++i) {
        if (position + 4 > f.size()) return;
        const auto seq = peer_wire::number(f[position]);
        const auto time = peer_wire::number(f[position + 1]);
        if (!seq || !time || f[position + 2].size() > max_text_size) return;
        Message item{f[position + 2], true, *time, f[position + 3], *seq};
        messages.push_back(std::move(item)); position += 4;
    }
    if (position >= f.size()) return;
    const auto file_count = peer_wire::number(f[position++]);
    if (!file_count || *file_count > 100 || position + *file_count * 4 != f.size()) return;
    std::vector<File> files;
    for (std::uint64_t i = 0; i < *file_count; ++i) {
        const auto size = peer_wire::number(f[position + 2]);
        if (!peer_wire::token(f[position]) || !valid_filename(f[position + 1]) || !size || *size > max_file_size) return;
        File file{f[position], f[position + 1], *size, {}};
        file.folder = f[position + 3] == "1";
        files.push_back(std::move(file)); position += 4;
    }
    std::unique_lock state_lock(mutex_);
    if (generation != generation_ || !session_.pairing(local)) return;
    if (outgoing && f[2] == "approved") session_.decide_pairing(local, true);
    const bool approved = f[2] == "approved" && session_.authorized(local);
    if (approved) {
        session_.touch(local);
        session_.merge_peer_clear(local, f[9]);
        const auto own = session_.snapshot(SnapshotPage::sync);
        if (own.permissions.text && f[6] == "1") {
            for (const auto& message : messages) if (message.sequence > last_message_) {
                if (session_.receive_peer_message(local, message)) last_message_ = message.sequence;
            }
        }
    }
    {
        state_.approved = approved; state_.online = true; state_.device = f[1];
        state_.verification = f[3]; state_.capture_ready = f[8] == "1";
        state_.permissions = {f[4] == "1", f[5] == "1", f[6] == "1", f[7] == "1"};
        state_.files = std::move(files);
        state_.status = approved ? "已连接 · 双向桌面会话" : "等待对方确认";
    }
    const bool revoke_cast = f[7] != "1";
    state_lock.unlock();
    if (revoke_cast && session_.snapshot(SnapshotPage::home).casting) session_.stop_cast();
}

void PeerLink::run() {
    for (;;) {
        bool connecting, attached;
        {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, 750ms, [&] { return stopping_ || connecting_; });
            if (stopping_) break;
            connecting = connecting_; attached = state_.attached;
        }
        try {
            if (connecting) connect();
            else if (attached) synchronize();
        } catch (const std::exception&) {
            std::lock_guard lock(mutex_); connecting_ = false; state_.online = false;
            state_.status = "网络请求失败，请检查地址与证书后重试";
        }
        if ((connecting || attached) && changed_) changed_();
    }
}

bool PeerLink::download(const File& file) {
    if (file.folder || !peer_wire::token(file.id) || !valid_filename(file.name) || file.size > max_file_size) return false;
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (state_.transferring || !state_.approved || !state_.online || !state_.permissions.download) return false;
        state_.transferring = true; state_.transferred = 0; state_.total = file.size;
        generation = generation_;
    }
    if (transfer_.joinable()) transfer_.join();
    cancel_ = false;
    transfer_ = std::thread([this, file, generation] { fetch_file(file, generation); });
    return true;
}
void PeerLink::cancel_transfer() { cancel_ = true; }
bool PeerLink::send(const std::filesystem::path& path) {
    return send(std::vector<std::filesystem::path>{path});
}

bool PeerLink::send(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty() || paths.size() > 100) return false;
    std::vector<std::filesystem::path> pending;
    std::uint64_t total = 0;
    for (const auto& path : paths) {
        std::error_code error;
        const auto absolute = std::filesystem::canonical(path, error);
        const auto version = error ? std::optional<SourceVersion>{} : SharedSource(absolute).version();
        if (!version || version->size > max_file_size || !valid_filename(path.filename().u8string())) return false;
        pending.push_back(absolute); total += version->size;
    }
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (state_.transferring || !state_.approved || !state_.online || !state_.permissions.upload) return false;
        state_.transferring = true; state_.transferred = 0; state_.total = total; state_.transfer_status = "等待对方接收确认";
        generation = generation_;
    }
    if (transfer_.joinable()) transfer_.join();
    cancel_ = false;
    transfer_ = std::thread([this, pending, generation] {
        for (std::size_t index = 0; index < pending.size() && !cancel_; ++index) {
            send_file(pending[index], generation);
        }
        { std::lock_guard lock(mutex_); state_.transferring = false; }
        if (changed_) changed_();
    });
    return true;
}

void PeerLink::send_file(const std::filesystem::path& path, std::uint64_t generation) {
    LanEndpoint endpoint;
    std::string token, local;
    { std::lock_guard lock(mutex_);
      if (generation != generation_) { cancel_ = true; return; }
      endpoint = endpoint_; token = remote_token_; local = local_token_; }
    auto remote = client(endpoint);
    auto headers = credentials(token);
    std::string offer_id;
    std::string detail;
    bool complete = false;
    try {
        SharedSource source(path);
        const auto version = source.version();
        if (!version) throw std::runtime_error("无法读取文件");
        headers.emplace("X-File-Name", httplib::encode_uri_component(path.filename().u8string()));
        headers.emplace("X-File-Size", std::to_string(version->size));
        const auto offered = remote->Post(endpoint.route + "/api/offers", headers, "", "text/plain");
        if (!offered || offered->status != 201) throw std::runtime_error("无法申请发送，请检查对方权限");
        offer_id = offered->get_header_value("X-Offer-ID");
        if (!peer_wire::token(offer_id)) throw std::runtime_error("对方版本不支持桌面发送");
        const auto deadline = Session::Clock::now() + std::chrono::minutes(5);
        bool approved = false;
        while (!cancel_ && Session::Clock::now() < deadline) {
            if (!session_.authorized(local)) throw std::runtime_error("本机已离开连接");
            const auto status = remote->Get(endpoint.route + "/api/desktop/offer/" + offer_id, credentials(token));
            const auto fields = status && status->status == 200 ? peer_wire::decode(status->body) : std::nullopt;
            if (fields && fields->size() == 2) {
                if ((*fields)[0] == "approved") { approved = true; break; }
                if ((*fields)[0] == "rejected") throw std::runtime_error("对方拒绝或取消了接收");
            }
            std::this_thread::sleep_for(150ms);
        }
        if (!approved || cancel_) throw std::runtime_error(cancel_ ? "发送已取消" : "等待对方确认超时");
        std::uint64_t offset = 0;
        do {
            if (cancel_ || !session_.authorized(local)) throw std::runtime_error("发送已取消或连接结束");
            if (!(source.version() == version)) throw std::runtime_error("源文件已变动，请重新选择");
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(4 * 1024 * 1024, version->size - offset));
            auto upload_headers = credentials(token);
            upload_headers.emplace("X-Upload-Offset", std::to_string(offset));
            const auto uploaded = remote->Put(endpoint.route + "/api/upload/" + offer_id, upload_headers, length,
                [&](std::size_t position, std::size_t requested, httplib::DataSink& sink) {
                    std::array<char, 64 * 1024> data{};
                    const auto count = std::min(requested, data.size());
                    return !cancel_ && source.read(offset + position, data.data(), count) && sink.write(data.data(), count);
                }, "application/octet-stream");
            if (!uploaded || uploaded->status != 201) throw std::runtime_error("发送中断，对方可能取消接收；可重新发送");
            offset += length;
            { std::lock_guard lock(mutex_); if (generation != generation_) { cancel_ = true; break; }
              state_.transferred = offset; state_.total = version->size;
              state_.transfer_status = "正在发送 · " + path.filename().u8string(); }
            if (changed_) changed_();
        } while (offset < version->size);
        if (cancel_) throw std::runtime_error("发送已取消");
        complete = true; detail = "对方已保存 · " + path.filename().u8string();
    } catch (const std::exception& error) { detail = error.what(); }
    if (!complete && !offer_id.empty()) remote->Delete(endpoint.route + "/api/offers/" + offer_id, credentials(token));
    { std::lock_guard lock(mutex_); if (generation == generation_) state_.transfer_status = detail; }
    if (changed_) changed_();
}


void PeerLink::fetch_file(File file, std::uint64_t generation) {
    LanEndpoint endpoint;
    std::string token, name, local;
    { std::lock_guard lock(mutex_);
      if (generation != generation_) { state_.transferring = false; return; }
      endpoint = endpoint_; token = remote_token_; name = state_.device; local = local_token_; }
    const auto directory = session_.snapshot(SnapshotPage::home).directory;
    const auto staging = directory / (".tempshare-download-" + random_token());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string detail;
    try {
        if (error) throw std::runtime_error("无法创建接收目录");
        const auto disk = std::filesystem::space(directory, error);
        if (error || disk.available < file.size + minimum_free_bytes) throw std::runtime_error("接收空间不足");
        std::ofstream output(staging, std::ios::binary);
        if (!output) throw std::runtime_error("无法写入接收目录");
        auto remote = client(endpoint);
        std::uint64_t written = 0;
        auto response = remote->Get(endpoint.route + "/api/download/" + file.id, credentials(token),
            [&](const httplib::Response& result) { return result.status == 200; },
            [&](const char* data, std::size_t size) {
                if (cancel_ || !session_.authorized(local) || size > file.size - written) return false;
                output.write(data, static_cast<std::streamsize>(size));
                if (!output) return false;
                written += size;
                { std::lock_guard lock(mutex_); state_.transferred = written; }
                if (changed_) changed_();
                return true;
            });
        output.close();
        if (!response || response->status != 200 || written != file.size || !output || cancel_ || !session_.authorized(local))
            throw std::runtime_error(cancel_ ? "下载已取消" : "下载中断，可重试");
        auto destination = directory / std::filesystem::u8path(file.name);
        for (unsigned i = 1; std::filesystem::exists(destination, error) && !error; ++i) {
            if (i > 10000) throw std::runtime_error("同名文件过多");
            const auto path = std::filesystem::u8path(file.name);
            destination = directory / (path.stem().u8string() + " (" + std::to_string(i) + ")" + path.extension().u8string());
        }
        if (error) throw std::runtime_error("无法确认目标文件");
        // hard_link 不覆盖并发创建的目标，成功后再删除临时文件。
        std::filesystem::create_hard_link(staging, destination, error);
        if (error) throw std::runtime_error("无法发布文件，请检查目录权限或重试");
        session_.record_download(destination, name);
        detail = "已保存 · " + destination.u8string();
    } catch (const std::exception& exception) { detail = exception.what(); }
    std::filesystem::remove(staging, error);
    { std::lock_guard lock(mutex_); state_.transferring = false;
      if (generation == generation_) state_.transfer_status = detail; }
    if (changed_) changed_();
}

void PeerLink::register_routes(httplib::Server& server) {
    server.Post("/api/desktop/pair", [this](const httplib::Request& request, httplib::Response& response) {
        const auto key = request.get_header_value("X-Desktop-Key");
        const auto id = request.get_header_value("X-Desktop-ID");
        { std::lock_guard lock(mutex_); if (id == device_id_) return failure(response, 409, "self_connection"); }
        const auto port = peer_wire::number(request.get_header_value("X-Desktop-Port"));
        const auto route = request.get_header_value("X-Desktop-Route");
        if (!peer_wire::token(key) || !peer_wire::token(id) || !port || !*port || *port > 65535 ||
            (!route.empty() && !(route.size() == 4 && route.rfind("/s/", 0) == 0 && route[3] >= '1' && route[3] <= '4')))
            return failure(response, 400, "invalid_peer");
        const auto local = session_.snapshot(SnapshotPage::home);
        if (local.pairing) {
            std::function<std::string()> admission;
            { std::lock_guard lock(mutex_); admission = available_route_; }
            const auto alternate = admission ? admission() : std::string{};
            if (!alternate.empty()) {
                response.status = 307; response.set_header("X-Desktop-Route", alternate); return;
            }
            return failure(response, 409, "device_busy");
        }
        if (!local.active) return failure(response, 403, "session_expired");
        auto invitation = request.get_header_value("X-Invitation");
        if (invitation.empty()) invitation = local.invitation; // 发现连接只提出待审批请求，绝不自动授权。
        const auto encoded = request.get_header_value("X-Device-Name");
        auto name = httplib::decode_uri(encoded);
        const auto pair = session_.pair(invitation, name);
        if (!pair) return failure(response, 403, "invalid_invitation");
        const auto source = request.remote_addr == "127.0.0.1" && request.has_header("X-LAN-Source") ?
            request.get_header_value("X-LAN-Source") : request.remote_addr;
        if (!is_private_ipv4(source) && !(loopback_ && source == "127.0.0.1")) {
            session_.disconnect(pair->token); return failure(response, 403, "private_lan_required");
        }
        {
            std::lock_guard lock(mutex_);
            endpoint_ = {source, static_cast<unsigned short>(*port), request.get_header_value("X-Desktop-Secure") == "1", route, {}};
            local_token_ = pair->token; remote_token_ = key;
            browser_key_ = random_token(); last_message_ = 0; ++generation_;
            cancel_ = true;
            const bool transferring = state_.transferring;
            state_ = {}; state_.transferring = transferring; state_.attached = true; state_.device = name; state_.device_id = id;
            state_.address = endpoint_.origin() + route; state_.verification = pair->verification;
            state_.status = "收到桌面连接请求";
        }
        response.status = 201;
        std::string own_id, own_name;
        { std::lock_guard lock(mutex_); own_id = device_id_; own_name = device_name_.empty() ? device_hostname() : device_name_; }
        response.set_content(peer_wire::encode({pair->token, pair->verification, own_name, own_id}),
                             "application/octet-stream");
        condition_.notify_all();
        if (changed_) changed_();
    });
    server.Get("/api/desktop/state", [this](const httplib::Request& request, httplib::Response& response) {
        const auto key = request.get_header_value("X-Desktop-Key");
        const auto pair = session_.pairing(key);
        if (!pair) return failure(response, 401, "session_ended");
        session_.touch(key);
        const auto state = session_.snapshot(SnapshotPage::all);
        const bool approved = pair->state == State::approved;
        const auto permissions = state.permissions;
        std::string own_name;
        { std::lock_guard lock(mutex_); own_name = device_name_.empty() ? device_hostname() : device_name_; }
        Fields fields{"1", own_name, std::string(state_name(pair->state)), pair->verification,
            permissions.download ? "1" : "0", permissions.upload ? "1" : "0", permissions.text ? "1" : "0",
            permissions.cast ? "1" : "0", approved && state.capture_ready ? "1" : "0", state.messages_cleared};
        std::vector<Message> messages;
        if (approved && permissions.text) for (const auto& message : state.messages)
            if (message.from_desktop && !message.local_only) messages.push_back(message);
        fields.push_back(std::to_string(messages.size()));
        for (const auto& message : messages) {
            fields.insert(fields.end(), {std::to_string(message.sequence), std::to_string(message.sent_at),
                                         message.text, message.attachment});
        }
        const auto files = approved && permissions.download ? state.shared : std::vector<File>{};
        fields.push_back(std::to_string(files.size()));
        for (const auto& file : files) fields.insert(fields.end(), {file.id, file.name, std::to_string(file.size), file.folder ? "1" : "0"});
        response.set_content(peer_wire::encode(fields), "application/octet-stream");
    });
    server.Get(R"(/api/desktop/offer/([0-9a-f]{32}))", [this](const httplib::Request& request, httplib::Response& response) {
        const auto offers = session_.offers(request.get_header_value("X-Desktop-Key"));
        if (!offers) return failure(response, 403, "approval_required");
        for (const auto& offer : *offers) if (offer.id == request.matches[1].str()) {
            response.set_content(peer_wire::encode({std::string(state_name(offer.state)), std::to_string(offer.transferred)}),
                                 "application/octet-stream"); return;
        }
        failure(response, 404, "offer_missing");
    });
    server.Post("/peer/authorize", [this](const httplib::Request& request, httplib::Response& response) {
        if (!loopback(request) || request.get_header_value("X-Peer-Key") != browser_key())
            return failure(response, 403, "local_authorization_required");
        response.set_header("Set-Cookie", "ts_peer_" + std::to_string(port_) + "=" + browser_key() +
            "; Path=" + public_route_ + "/peer; HttpOnly; SameSite=Strict" + (secure_ ? "; Secure" : ""));
        response.status = 204;
    });
    for (const auto* method : {"GET", "HEAD", "POST", "PUT", "DELETE"}) {
        auto handler = [this](const httplib::Request& request, httplib::Response& response) { bridge(request, response); };
        if (std::string(method) == "GET") server.Get(R"(/peer/api/.*)", handler);
        else if (std::string(method) == "POST") server.Post(R"(/peer/api/.*)", handler);
        else if (std::string(method) == "PUT") server.Put(R"(/peer/api/.*)", handler);
        else if (std::string(method) == "DELETE") server.Delete(R"(/peer/api/.*)", handler);
    }
}

bool PeerLink::bridge(const httplib::Request& request, httplib::Response& response) {
    if (!loopback(request) || cookie(request, "ts_peer_" + std::to_string(port_)) != browser_key()) {
        failure(response, 403, "local_authorization_required"); return false;
    }
    LanEndpoint endpoint;
    std::string token, local;
    { std::lock_guard lock(mutex_); endpoint = endpoint_; token = remote_token_; local = local_token_; }
    if (token.empty()) { failure(response, 401, "session_ended"); return false; }
    if (request.path.rfind("/peer/api/cast/", 0) == 0 && !session_.permissions().cast) {
        failure(response, 403, "cast_disabled"); return false;
    }
    auto headers = credentials(token);
    for (const auto& header : request.headers) {
        if (header.first.rfind("X-Cast-", 0) == 0 || header.first.rfind("X-Upload-", 0) == 0 ||
            header.first == "X-File-Name" || header.first == "X-File-Size" || header.first == "X-Batch" ||
            header.first == "X-Relative-Directory" || header.first == "X-Image" ||
            header.first == "Range" || header.first == "Content-Type") headers.insert(header);
    }
    const auto path = endpoint.route + request.target.substr(5);
    auto remote = client(endpoint);
    const bool download = request.method == "GET" && (path.find("/download/") != std::string::npos ||
        path.find("/attachment/") != std::string::npos || path.find("/zip/") != std::string::npos);
    if (download) {
        const auto head = remote->Head(path, headers);
        if (!head || head->status >= 400) {
            failure(response, head ? head->status : 502, "download_unavailable"); return false;
        }
        response.status = head->status;
        for (const auto& name : {"Content-Disposition", "Content-Type", "Content-Range", "Accept-Ranges"})
            if (head->has_header(name)) response.set_header(name, head->get_header_value(name));
        response.set_chunked_content_provider(head->get_header_value("Content-Type"),
            [endpoint, path, headers](std::size_t, httplib::DataSink& sink) {
                auto source = client(endpoint);
                const auto result = source->Get(path, headers,
                    [](const httplib::Response& item) { return item.status >= 200 && item.status < 300; },
                    [&](const char* data, std::size_t size) { return sink.write(data, size); });
                if (!result) return false;
                sink.done(); return true;
            });
        return true;
    }
    httplib::Request outbound;
    outbound.method = request.method; outbound.path = path; outbound.headers = headers; outbound.body = request.body;
    std::string body;
    outbound.content_receiver = [&](const char* data, std::size_t size, std::uint64_t, std::uint64_t) {
        if (size > peer_wire::limit - body.size()) return false;
        body.append(data, size); return true;
    };
    const auto result = remote->send(outbound);
    if (!result) { failure(response, 502, "peer_unreachable"); return false; }
    response.status = result->status;
    if (request.method == "POST" && request.path == "/peer/api/text" && result->status == 201) {
        session_.record_peer_sent(local, request.body);
    }
    response.set_content(body, result->get_header_value("Content-Type"));
    for (const auto& name : {"Content-Length", "Content-Disposition", "Content-Range", "Accept-Ranges", "Retry-After"})
        if (result->has_header(name) && request.method == "HEAD") response.set_header(name, result->get_header_value(name));
    return true;
}

} // namespace tempshare
