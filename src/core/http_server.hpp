#pragma once

#include "core/discovery.hpp"

#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; struct Request; struct Response; }

namespace tempshare {

class Session;
class PeerLink;

/** Serves the bundled browser UI and the file API. Owns and joins all network workers. */
class HttpServer {
public:
    HttpServer(Session& session, const std::string& certificate = {}, const std::string& key = {},
               std::function<void()> changed = {});
    ~HttpServer();

    bool start(unsigned short preferred_port, bool loopback = false);
    void stop();
    unsigned short port() const { return port_; }
    bool secure() const { return secure_; }
    std::string url(const std::string& address) const;
    bool probe(const std::string& address) const;
    std::vector<LocalNetwork> networks() const;
    bool update_networks(std::vector<LocalNetwork> networks);
    PeerLink& peer();
    const PeerLink& peer() const;
    void configure_gateway(const std::vector<HttpServer*>& sessions);

private:
    void forward_session(const httplib::Request& request, httplib::Response& response);
    std::string available_session();
    Session& session_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<PeerLink> peer_;
    std::thread thread_;
    unsigned short port_ = 0;
    bool secure_;
    mutable std::mutex networks_mutex_;
    std::vector<LocalNetwork> networks_;
    std::vector<HttpServer*> routes_;
    std::string gateway_key_;
    unsigned short public_port_ = 0;
    std::string public_route_;
};

}  // namespace tempshare
