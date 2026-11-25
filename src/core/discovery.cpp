#include "core/discovery.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tempshare {
namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned short discovery_port = 47871;
constexpr char multicast[] = "239.255.88.17";
constexpr std::string_view protocol = "TempShare/1 ";
#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket invalid_socket = INVALID_SOCKET;
constexpr auto platform = "Windows";
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket invalid_socket = -1;
#ifdef __APPLE__
constexpr auto platform = "macOS";
#else
constexpr auto platform = "Linux";
#endif
#endif

struct Datagram {
    Socket fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ~Datagram() {
        if (fd == invalid_socket) return;
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }

    template<typename Value>
    void option(int level, int name, const Value& value) const {
        setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
    }
};

std::string format_address(const in_addr& address) {
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, text, sizeof(text))) return {};
    return text;
}

bool private_address(std::uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == 0xac1 ||
           (ip >> 16) == 0xc0a8;
}

void add_network(std::vector<LocalNetwork>& networks, const sockaddr* address, std::uint32_t mask) {
    if (!address || address->sa_family != AF_INET) return;
    const auto native = reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
    const auto ip = ntohl(native.s_addr);
    if (!private_address(ip) || !mask) return;
    const auto found = std::find_if(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
        return network.ip == ip;
    });
    if (found == networks.end()) {
        networks.push_back({format_address(native), ip, mask, {}, false});
    }
}

std::string hostname() {
    char buffer[64]{};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0) return "TempShare";
    std::string name = buffer;
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) c = '-';
    }
    return name.empty() ? "TempShare" : name;
}

std::optional<Peer> announcement(std::string_view packet, std::string address) {
    if (packet.substr(0, protocol.size()) != protocol) return {};
    packet.remove_prefix(protocol.size());
    const auto gap = packet.find(' ');
    if (gap == std::string_view::npos) return {};
    unsigned short port = 0;
    const auto end = packet.data() + gap;
    const auto parsed = std::from_chars(packet.data(), end, port);
    if (parsed.ec != std::errc() || parsed.ptr != end || !port) return {};
    packet.remove_prefix(gap + 1);
    if (packet.size() < 4 || (packet[0] != 's' && packet[0] != 'h') || packet[1] != ' ') return {};
    const bool secure = packet.front() == 's';
    packet.remove_prefix(2);
    const auto name_start = packet.find(' ');
    if (name_start == std::string_view::npos) return {};
    const auto os = packet.substr(0, name_start);
    const auto name = packet.substr(name_start + 1);
    if ((os != "Windows" && os != "macOS" && os != "Linux") || name.empty() || name.size() > 63 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c <= 126; })) {
        return {};
    }
    return Peer{std::move(address), port, std::string(name), std::string(os), secure};
}

}  // namespace

std::string device_hostname() { return hostname(); }

std::string Peer::url() const {
    return std::string(secure ? "https://" : "http://") + address + ':' + std::to_string(port) + '/';
}

bool LocalNetwork::contains(const std::string& other) const {
    in_addr native{};
    if (inet_pton(AF_INET, other.c_str(), &native) != 1) return false;
    const auto candidate = ntohl(native.s_addr);
    return private_address(candidate) && (candidate & mask) == (ip & mask);
}

bool is_private_ipv4(const std::string& address) {
    in_addr native{};
    return inet_pton(AF_INET, address.c_str(), &native) == 1 && private_address(ntohl(native.s_addr));
}

std::vector<LocalNetwork> lan_networks() {
    std::vector<LocalNetwork> networks;
#ifdef _WIN32
    ULONG size = 15000;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    constexpr auto flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                           GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    auto status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (status == NO_ERROR) {
        std::vector<std::pair<unsigned, IP_ADAPTER_ADDRESSES*>> ordered;
        for (auto* entry = adapters; entry; entry = entry->Next) {
            if (entry->OperStatus != IfOperStatusUp || entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                entry->IfType == IF_TYPE_TUNNEL || entry->IfType == IF_TYPE_PPP) continue;
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = entry->Luid;
            const bool physical = GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
            const unsigned priority = (physical ? 0u : 2u) + (entry->FirstGatewayAddress ? 0u : 1u);
            ordered.emplace_back(priority, entry);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            if (left.first != right.first) return left.first < right.first;
            return left.second->Ipv4Metric < right.second->Ipv4Metric;
        });
        for (const auto& choice : ordered) {
            for (auto* unicast = choice.second->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                const auto prefix = unicast->OnLinkPrefixLength;
                if (prefix > 0 && prefix <= 32) {
                    add_network(networks, unicast->Address.lpSockaddr, 0xffffffffu << (32 - prefix));
                    const auto local = format_address(reinterpret_cast<const sockaddr_in*>(
                        unicast->Address.lpSockaddr)->sin_addr);
                    const auto found = std::find_if(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
                        return network.address == local;
                    });
                    if (found != networks.end()) {
                        const auto* name = choice.second->FriendlyName;
                        const auto length = name ? WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr) : 0;
                        if (length > 1) {
                            found->interface_name.resize(static_cast<std::size_t>(length));
                            WideCharToMultiByte(CP_UTF8, 0, name, -1, found->interface_name.data(), length, nullptr, nullptr);
                            found->interface_name.pop_back();
                        }
                        found->wireless = choice.second->IfType == IF_TYPE_IEEE80211;
                    }
                }
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
            if ((entry->ifa_flags & IFF_UP) && !(entry->ifa_flags & IFF_POINTOPOINT) &&
                entry->ifa_netmask && entry->ifa_netmask->sa_family == AF_INET) {
                const auto mask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr;
                add_network(networks, entry->ifa_addr, ntohl(mask.s_addr));
                const auto local = format_address(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr);
                const auto found = std::find_if(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
                    return network.address == local;
                });
                if (found != networks.end()) {
                    found->interface_name = entry->ifa_name;
                    found->wireless = std::string_view(entry->ifa_name).rfind("wlan", 0) == 0 ||
                        std::string_view(entry->ifa_name).rfind("wlp", 0) == 0;
                }
            }
        }
        freeifaddrs(interfaces);
    }
    // UDP connect selects the system route without sending a packet.
    Datagram route;
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);
    if (route.fd != invalid_socket && connect(route.fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in chosen{};
        AddressLength chosen_size = sizeof(chosen);
        if (getsockname(route.fd, reinterpret_cast<sockaddr*>(&chosen), &chosen_size) == 0) {
            const auto ip = ntohl(chosen.sin_addr.s_addr);
            const auto found = std::find_if(networks.begin(), networks.end(), [ip](const LocalNetwork& network) {
                return network.ip == ip;
            });
            if (found != networks.end()) std::rotate(networks.begin(), found, found + 1);
        }
    }
#endif
    return networks;
}

bool permits_private_lan_peer(const std::vector<LocalNetwork>& interfaces,
                              const std::string& local, const std::string& remote) {
    return is_private_ipv4(remote) &&
        std::any_of(interfaces.begin(), interfaces.end(), [&](const LocalNetwork& network) {
            return network.address == local;
        });
}

Discovery::Discovery(unsigned short port, bool secure, std::function<std::vector<LocalNetwork>()> networks,
                     std::function<void()> changed)
    : port_(port), secure_(secure), networks_(std::move(networks)),
      changed_(std::move(changed)), worker_(&Discovery::run, this) {}

Discovery::~Discovery() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

std::vector<Peer> Discovery::peers() const {
    std::lock_guard lock(mutex_);
    std::vector<Peer> result;
    for (const auto& item : peers_) result.push_back(item.peer);
    return result;
}

void Discovery::run() {
    while (running_) {
        const auto networks = networks_();
        if (!networks.empty()) announce(networks);
        if (!running_) break;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            changed = !peers_.empty();
            peers_.clear();
        }
        if (changed && changed_) changed_();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void Discovery::announce(const std::vector<LocalNetwork>& networks) {
    Datagram socket;
    if (socket.fd == invalid_socket) return;
    socket.option(SOL_SOCKET, SO_REUSEADDR, 1);
#ifndef _WIN32
#ifdef SO_REUSEPORT
    socket.option(SOL_SOCKET, SO_REUSEPORT, 1);
#endif
#endif
    socket.option(IPPROTO_IP, IP_MULTICAST_TTL, static_cast<unsigned char>(1));
    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_port = htons(discovery_port);
    if (bind(socket.fd, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) != 0) return;

    sockaddr_in group = bound;
    inet_pton(AF_INET, multicast, &group.sin_addr);
    for (const auto& network : networks) {
        ip_mreq membership{};
        membership.imr_multiaddr = group.sin_addr;
        inet_pton(AF_INET, network.address.c_str(), &membership.imr_interface);
        socket.option(IPPROTO_IP, IP_ADD_MEMBERSHIP, membership);
    }

    const std::string packet = std::string(protocol) + std::to_string(port_) + (secure_ ? " s " : " h ") +
                               platform + ' ' + hostname();
    auto next_send = Clock::now();
    while (running_ && networks_() == networks) {
        const auto now = Clock::now();
        if (refresh_.exchange(false) || now >= next_send) {
            for (const auto& network : networks) {
                in_addr local{};
                inet_pton(AF_INET, network.address.c_str(), &local);
                socket.option(IPPROTO_IP, IP_MULTICAST_IF, local);
                sendto(socket.fd, packet.data(), static_cast<int>(packet.size()), 0,
                       reinterpret_cast<sockaddr*>(&group), sizeof(group));
            }
            next_send = now + std::chrono::seconds(3);
        }
        bool changed = false;
#ifdef _WIN32
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket.fd, &readable);
        timeval timeout{0, 200000};
        const bool readable_socket = select(0, &readable, nullptr, nullptr, &timeout) > 0;
#else
        pollfd readable{socket.fd, POLLIN, 0};
        const bool readable_socket = poll(&readable, 1, 200) > 0 && (readable.revents & POLLIN);
#endif
        if (readable_socket) {
            char incoming[160]{};
            sockaddr_in source{};
            AddressLength size = sizeof(source);
            const auto count = recvfrom(socket.fd, incoming, sizeof(incoming), 0,
                                        reinterpret_cast<sockaddr*>(&source), &size);
            const auto address = format_address(source.sin_addr);
            if (count > 0 && std::any_of(networks.begin(), networks.end(), [&](const LocalNetwork& network) {
                return network.contains(address);
            })) {
                auto peer = announcement({incoming, static_cast<std::size_t>(count)}, address);
                if (peer && (peer->port != port_ || std::none_of(networks.begin(), networks.end(),
                        [&](const LocalNetwork& network) { return network.address == address; }))) {
                    std::lock_guard lock(mutex_);
                    const auto found = std::find_if(peers_.begin(), peers_.end(), [&](const Seen& seen) {
                        return seen.peer.address == address && seen.peer.port == peer->port;
                    });
                    if (found != peers_.end()) {
                        changed = found->peer.name != peer->name || found->peer.platform != peer->platform ||
                                  found->peer.secure != peer->secure;
                        *found = Seen{*peer, now};
                    } else if (peers_.size() < 64) {
                        peers_.push_back(Seen{*peer, now});
                        changed = true;
                    }
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            const auto stale = std::remove_if(peers_.begin(), peers_.end(), [now](const Seen& seen) {
                return now - seen.last > std::chrono::seconds(10);
            });
            changed = changed || stale != peers_.end();
            peers_.erase(stale, peers_.end());
        }
        if (changed && changed_) changed_();
    }
}

void Discovery::refresh() { refresh_ = true; }

}  // namespace tempshare
