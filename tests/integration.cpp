#include "core/discovery.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"

#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void require(bool passed, const char* description) {
    if (!passed) throw std::runtime_error(description);
}

void expect(const httplib::Result& result, int status, const char* description) {
    if (!result) {
        std::cerr << description << ": " << httplib::to_string(result.error()) << '\n';
    } else if (result->status != status) {
        std::cerr << description << ": expected " << status << ", got " << result->status << '\n';
    }
    require(result && result->status == status, description);
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "open received file");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Sandbox {
    fs::path path = fs::temp_directory_path() / ("tempshare-test-" + tempshare::random_token());
    Sandbox() { require(fs::create_directory(path), "create test directory"); }
    ~Sandbox() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void roundtrip(const fs::path& path) {
    tempshare::Session session(path / "received");
    tempshare::HttpServer server(session);
    require(server.start(0, true), "start loopback HTTP server");

    httplib::Client browser("127.0.0.1", server.port());
    browser.set_read_timeout(2);
    expect(browser.Get("/"), 200, "serve bundled web page");
    expect(browser.Get("/styles.css"), 200, "serve bundled styles");
    expect(browser.Get("/app.js"), 200, "serve bundled browser code");
    expect(browser.Get("/api/files"), 403, "reject unauthenticated file listing");

    session.start();
    const auto invitation = session.snapshot();
    require(invitation.active, "new session active");
    const auto wrong_invite = browser.Post("/api/pair", {{"X-TempShare", "1"}, {"X-Invitation", "wrong"}}, "", "text/plain");
    expect(wrong_invite, 403, "reject wrong invitation");
    expect(browser.Post("/api/pair", {{"X-Invitation", invitation.code}}, "", "text/plain"),
           403, "require browser intent header");

    const auto pairing = browser.Post("/api/pair",
        {{"X-TempShare", "1"}, {"X-Invitation", invitation.invitation}}, "", "text/plain");
    expect(pairing, 201, "create QR pairing request");
    const auto cookie = pairing->get_header_value("Set-Cookie");
    require(cookie.find("HttpOnly") != std::string::npos, "session cookie is HttpOnly");
    const httplib::Headers auth{{"Cookie", cookie.substr(0, cookie.find(';'))}};
    auto mutation = auth;
    mutation.emplace("X-TempShare", "1");
    const auto awaiting = session.snapshot().pairing;
    require(awaiting && awaiting->state == tempshare::State::pending, "request awaits desktop approval");

    expect(browser.Get("/api/pair", auth), 200, "show pending verification to browser");
    expect(browser.Get("/api/files", auth), 403, "pending browser cannot list files");
    require(session.decide_pairing(awaiting->token, true), "approve browser on desktop");
    require(!session.pair(invitation.code), "second browser cannot take over a session");

    const auto outbound = path / fs::u8path(u8"分享 test.txt");
    std::ofstream(outbound, std::ios::binary) << "shared content";
    require(session.share(outbound), "add outbound file from desktop");
    const auto shared = session.snapshot().shared.front();
    const auto listing = browser.Get("/api/files", auth);
    expect(listing, 200, "show authorized files");
    require(listing->body.find(shared.id) != std::string::npos, "listing contains desktop file");
    const auto download_path = "/api/download/" + shared.id;
    const auto download = browser.Get(download_path, auth);
    expect(download, 200, "download desktop file");
    require(download->body == "shared content", "download bytes are intact");
    require(download->get_header_value("Content-Disposition").find("%E5%88%86") != std::string::npos,
            "download preserves Unicode filename");
    auto range = auth;
    range.emplace("Range", "bytes=0-5");
    const auto partial = browser.Get(download_path, range);
    expect(partial, 206, "stream byte-range download");
    require(partial->body == "shared", "range bytes are intact");

    auto invalid = mutation;
    invalid.emplace("X-File-Name", "%2E%2E%2Foutside.txt");
    invalid.emplace("X-File-Size", "4");
    expect(browser.Post("/api/offers", invalid, "", "text/plain"), 400, "reject unsafe received filename");

    const auto offer_upload = [&](const std::string& payload, bool approve) {
        auto offer = mutation;
        offer.emplace("X-File-Name", "%E7%85%A7%E7%89%87.txt");
        offer.emplace("X-File-Size", std::to_string(payload.size()));
        expect(browser.Post("/api/offers", offer, "", "text/plain"), 201, "create upload request");
        const auto id = session.snapshot().offers.back().id;
        const auto upload_path = "/api/upload/" + id;
        expect(browser.Put(upload_path, mutation, payload, "application/octet-stream"),
               409, "desktop approval required before file is written");
        require(session.decide_offer(id, approve), "desktop decides upload");
        const auto result = browser.Put(upload_path, mutation, payload, "application/octet-stream");
        expect(result, approve ? 201 : 409, approve ? "upload approved file" : "reject denied upload");
    };
    offer_upload("same", true);
    offer_upload("size", true);
    offer_upload("", true);
    offer_upload("denied", false);
    const auto received = session.snapshot().received;
    require(received.size() == 3 && received[0].path != received[1].path, "same names preserve both files");
    require(read(received[0].path) == "same" && read(received[1].path) == "size", "upload bytes are intact");
    require(read(received[2].path).empty(), "zero-byte file supported");
    for (const auto& file : fs::directory_iterator(received.front().path.parent_path())) {
        require(file.path().extension() != ".part", "clean temporary upload artifacts");
    }

    session.unshare(shared.id);
    expect(browser.Get(download_path, auth), 404, "removing file revokes download");
    session.end();
    expect(browser.Get("/api/pair", auth), 401, "ending session revokes cookie");
    expect(browser.Get("/api/files", auth), 403, "ending session revokes file access");

    session.start();
    const auto code = session.snapshot().code;
    const auto manual = browser.Post("/api/pair", {{"X-TempShare", "1"}, {"X-Invitation", code}}, "", "text/plain");
    expect(manual, 201, "six-digit manual pairing");
    require(session.decide_pairing(session.snapshot().pairing->token, false), "desktop rejects browser");
    const auto rejected_cookie = manual->get_header_value("Set-Cookie");
    expect(browser.Get("/api/pair", httplib::Headers{{"Cookie", rejected_cookie.substr(0, rejected_cookie.find(';'))}}),
           401, "rejected browser lost access");
    require(!session.pair(code), "rejected invitation revoked");
}

void session_lifetime(const fs::path& path) {
    tempshare::Session session(path, {}, 80ms);
    session.start();
    auto pair = session.pair(session.snapshot().invitation);
    require(pair && session.decide_pairing(pair->token, true), "approve short-lived session");
    const auto offer = session.offer(pair->token, "cancelled.txt", 1);
    require(offer && session.decide_offer(offer->id, true), "approve cancellable upload");
    const auto upload = session.begin_upload(pair->token, offer->id, 1);
    require(upload && session.cancel_offer(pair->token, offer->id), "cancel pending write");
    require(!session.finish_upload(pair->token, offer->id, *upload), "cancelled upload cannot commit");
    std::this_thread::sleep_for(100ms);
    require(!session.authorized(pair->token) && !session.snapshot().active, "session expires");
}

void network_changes(const fs::path& path) {
    const auto networks = tempshare::lan_networks();
    if (networks.empty()) return;
    tempshare::Session session(path);
    tempshare::HttpServer server(session);
    require(server.start(0), "start LAN listener");
    httplib::Client peer(networks.front().address, server.port());
    peer.set_connection_timeout(2);
    peer.set_read_timeout(2);
    expect(peer.Get("/"), 200, "serve the current LAN interface");
    require(server.update_networks({}), "remove a disconnected LAN interface");
    expect(peer.Get("/"), 403, "stop serving the disconnected LAN interface");
    require(server.update_networks(networks), "restore a reconnected LAN interface");
    expect(peer.Get("/"), 200, "serve the reconnected LAN interface without restart");
}

}  // namespace

int main() {
    try {
        Sandbox scratch;
        roundtrip(scratch.path);
        session_lifetime(scratch.path);
        network_changes(scratch.path);
        const tempshare::LocalNetwork lan{"192.168.70.31", 0xc0a8461f, 0xffffff00};
        require(lan.contains("192.168.70.22") && !lan.contains("192.168.71.22") &&
                !lan.contains("8.8.8.8"), "accept only the IPv4 LAN subnet");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
