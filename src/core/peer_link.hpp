#pragma once

#include "core/session.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; struct Request; struct Response; }

namespace tempshare {

/** @brief 私有 IPv4 或局域网主机名，主机名在后台解析后仅连接私有地址；fragment 为可选邀请码。 */
struct LanEndpoint {
    std::string host;
    unsigned short port = 8787;
    bool secure = false;
    std::string route;
    std::string invitation;
    std::string origin() const;
};
std::optional<LanEndpoint> parse_lan_endpoint(std::string value, bool allow_loopback = false);

/** @brief 桌面连接快照；密钥仅保存在原生进程，UI 不接触远端凭据。 */
struct PeerLinkState {
    bool attached = false;
    bool outgoing = false;
    bool online = false;
    bool approved = false;
    bool capture_ready = false;
    std::string device;
    std::string device_id;
    std::string address;
    std::string verification;
    std::string status;
    std::string transfer_status;
    std::vector<File> files;
    Permissions permissions;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    bool transferring = false;
};

/** @brief 双向原生会话桥；拥有并回收轮询与文件任务线程，Session 必须比本对象活得更久。 */
class PeerLink {
public:
    PeerLink(Session& session, std::function<void()> changed);
    ~PeerLink();
    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;
    void start(unsigned short port, bool secure, bool loopback = false);
    void stop();
    void register_routes(httplib::Server& server);
    bool join(const std::string& address, const std::vector<std::string>& alternatives = {}, const std::string& expected_device = {});
    void leave();
    PeerLinkState snapshot() const;
    bool download(const File& file);
    bool send(const std::filesystem::path& path);
    bool send(const std::vector<std::filesystem::path>& paths);
    void cancel_transfer();
    std::string browser_key() const;
    void set_public_endpoint(unsigned short port, std::string route);
    void set_admission(std::function<std::string()> available_route);
    void set_identity(std::string id, std::string name);

private:
    void run();
    void synchronize();
    void connect();
    void fetch_file(File file, std::uint64_t generation);
    void send_file(const std::filesystem::path& path, std::uint64_t generation);
    bool bridge(const httplib::Request& request, httplib::Response& response);
    Session& session_;
    std::function<void()> changed_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    bool connecting_ = false;
    unsigned short port_ = 0;
    bool secure_ = false;
    bool loopback_ = false;
    std::string public_route_;
    std::string local_token_;
    std::string remote_token_;
    std::string browser_key_;
    std::string device_id_;
    std::string device_name_;
    LanEndpoint endpoint_;
    std::vector<LanEndpoint> alternative_endpoints_;
    std::string expected_device_;
    PeerLinkState state_;
    std::uint64_t last_message_ = 0;
    std::uint64_t generation_ = 0;
    std::thread worker_;
    std::thread transfer_;
    std::atomic<bool> cancel_{false};
    std::function<std::string()> available_route_;
};

} // namespace tempshare
