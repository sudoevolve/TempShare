#pragma once

#include "core/discovery.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace tempshare {

class Session;

/** Serves the bundled browser UI and the file API. Owns and joins all network workers. */
class HttpServer {
public:
    HttpServer(Session& session, const std::string& certificate = {}, const std::string& key = {});
    ~HttpServer();

    bool start(unsigned short preferred_port, bool loopback = false);
    void stop();
    unsigned short port() const { return port_; }
    std::string url(const std::string& address) const;
    bool probe(const std::string& address) const;
    std::vector<LocalNetwork> networks() const;
    bool update_networks(std::vector<LocalNetwork> networks);

private:
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    unsigned short port_ = 0;
    bool secure_;
    mutable std::mutex networks_mutex_;
    std::vector<LocalNetwork> networks_;
};

}  // namespace tempshare
