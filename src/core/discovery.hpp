#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tempshare {

struct Peer {
    std::string address;
    unsigned short port;
    std::string name;
    std::string platform;
    bool secure;
    std::string id;
    std::vector<std::string> addresses;

    std::string url() const;
};

struct LocalNetwork {
    std::string address;
    std::uint32_t ip;
    std::uint32_t mask;
    std::string interface_name;
    bool wireless = false;
    bool contains(const std::string& other) const;
    bool operator==(const LocalNetwork& other) const { return ip == other.ip && mask == other.mask; }
};

std::vector<LocalNetwork> lan_networks();
std::string device_hostname();
bool is_private_ipv4(const std::string& address);
std::vector<std::string> resolve_lan_addresses(const std::string& hostname);
bool permits_private_lan_peer(const std::vector<LocalNetwork>& interfaces,
                              const std::string& local, const std::string& remote);

class Discovery {
public:
    Discovery(unsigned short port, bool secure, std::function<std::vector<LocalNetwork>()> networks,
              std::function<void()> changed, std::string id = {}, std::string name = {});
    ~Discovery();
    std::vector<Peer> peers() const;
    void refresh();
    std::string status() const;
    void set_name(std::string name);

private:
    void run();
    void announce(const std::vector<LocalNetwork>& networks);

    struct Seen {
        Peer peer;
        std::chrono::steady_clock::time_point last;
    };

    unsigned short port_;
    bool secure_;
    std::function<std::vector<LocalNetwork>()> networks_;
    std::function<void()> changed_;
    std::atomic<bool> running_{true};
    std::atomic<bool> refresh_{false};
    mutable std::mutex mutex_;
    std::vector<Seen> peers_;
    std::thread worker_;
    std::string id_;
    std::string name_;
    std::string status_ = "正在扫描本地网络";
};

}  // namespace tempshare
