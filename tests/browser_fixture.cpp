#include "core/http_server.hpp"
#include "core/peer_link.hpp"
#include "core/session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("tempshare-browser-" + tempshare::random_token());
    fs::create_directories(scratch);
    int result = 0;
    try {
        tempshare::Session primary(scratch / "primary"), host(scratch / "host"), guest(scratch / "guest");
        tempshare::HttpServer entry(primary), service(host), client(guest);
        if (!entry.start(0, true) || !service.start(0, true) || !client.start(0, true)) return 1;
        entry.configure_gateway({&entry, &service});
        primary.start(); host.start(); guest.start();
        const auto busy = primary.pair(primary.snapshot().invitation, "occupied");
        primary.decide_pairing(busy->token, true);
        std::ofstream(scratch / "fixture.txt") << "browser integration file";
        host.share(scratch / "fixture.txt");
        std::cout << "{\"entry\":" << entry.port() << ",\"guest\":" << client.port() << "}" << std::endl;
        for (std::string command; std::getline(std::cin, command);) {
            if (command == "quit") break;
            if (command == "join") client.peer().join(entry.url("127.0.0.1"));
            if (command == "approve") {
                const auto pending = host.snapshot().pairing;
                if (pending) host.decide_pairing(pending->token, true);
            }
            if (command == "message") host.send_text("native desktop message");
            if (command == "permissions") {
                auto permissions = host.permissions(); permissions.text = !permissions.text; host.set_permissions(permissions);
            }
            if (command == "approve-files") {
                for (const auto& offer : host.snapshot().offers) host.decide_offer(offer.id, true);
            }
            const auto host_state = host.snapshot(), guest_state = guest.snapshot();
            const auto link = client.peer().snapshot();
            const auto host_key = command == "capture" ? host.open_cast() : std::string{};
            const auto guest_key = command == "reverse" ? guest.open_cast() : std::string{};
            std::cout << "{\"approved\":" << (link.approved ? "true" : "false") <<
                ",\"hostMessages\":" << host_state.messages.size() <<
                ",\"guestMessages\":" << guest_state.messages.size() <<
                ",\"received\":" << host_state.received.size() <<
                ",\"hostKey\":" << tempshare::json_string(host_key) <<
                ",\"guestKey\":" << tempshare::json_string(guest_key) <<
                ",\"hostPeerKey\":" << tempshare::json_string(service.peer().browser_key()) <<
                ",\"guestPeerKey\":" << tempshare::json_string(client.peer().browser_key()) << "}" << std::endl;
        }
        entry.stop(); service.stop(); client.stop();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return result;
}
