#include "core/discovery.hpp"
#include "core/folder_archive.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"

#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <future>
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
    std::ofstream(outbound, std::ios::binary | std::ios::trunc) << "changed content";
    expect(browser.Get(download_path, auth), 409, "changed shared file is rejected");
    const auto previous = path / "previous.txt";
    fs::rename(outbound, previous);
    std::ofstream(outbound, std::ios::binary) << "shared content";
    fs::last_write_time(outbound, fs::last_write_time(previous));
    expect(browser.Get(download_path, auth), 409, "same-size replacement must fail identity verification");

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

void lan_features(const fs::path& path) {
    tempshare::Session session(path / "chunked");
    tempshare::HttpServer server(session);
    require(server.start(0, true), "start feature server");
    httplib::Client browser("127.0.0.1", server.port());
    browser.set_read_timeout(3);
    session.start();
    const auto invitation = session.snapshot().invitation;
    const auto pair = session.pair(invitation, "Test phone");
    require(pair && session.decide_pairing(pair->token, true), "approve feature browser");
    const std::string cookie = "ts_" + std::to_string(server.port()) + "=" + pair->token;
    const httplib::Headers auth{{"Cookie", cookie}};
    auto post = auth;
    post.emplace("X-TempShare", "1");

    expect(browser.Post("/api/text", {{"X-TempShare", "1"}}, "hello", "text/plain"),
           403, "unauthorized browser cannot send text");
    require(session.send_text("https://example.test/path"), "desktop sends a link");
    expect(browser.Post("/api/text", post, u8"中文\ncode()", "text/plain"), 201, "browser sends multiline text");
    const auto entries = browser.Get("/api/text", auth);
    expect(entries, 200, "read text history");
    require(entries->body.find("code()") != std::string::npos && session.snapshot().messages.size() == 2,
            "both directions share the same history");
    expect(browser.Post("/api/text", post, std::string(tempshare::max_text_size + 1, 'x'), "text/plain"),
           400, "bound text request size");
    session.set_permissions({true, true, false});
    expect(browser.Get("/api/text", auth), 403, "revoke text permission");
    require(session.snapshot().messages.empty(), "revoking text clears history");

    const auto offer = session.offer(pair->token, "resume.txt", 8);
    require(offer && session.decide_offer(offer->id, true), "approve resumable upload");
    const auto upload = "/api/upload/" + offer->id;
    auto first = post;
    first.emplace("X-Upload-Offset", "0");
    expect(browser.Put(upload, first, "1234", "application/octet-stream"), 201, "commit first chunk");
    require(session.snapshot().offers[0].transferred == 4 && session.snapshot().received.empty(),
            "hold partial uploads out of receive history");
    expect(browser.Put(upload, first, "1234", "application/octet-stream"), 409, "reject duplicate offset");
    auto second = post;
    second.emplace("X-Upload-Offset", "4");
    expect(browser.Put(upload, second, "5678", "application/octet-stream"), 201, "resume and commit file");
    require(read(session.snapshot().received.back().path) == "12345678", "resumed bytes are intact");
    const auto cancelled = session.offer(pair->token, "cancelled.txt", 8);
    require(cancelled && session.decide_offer(cancelled->id, true), "approve cancellable chunked upload");
    const auto cancel_path = "/api/upload/" + cancelled->id;
    expect(browser.Put(cancel_path, first, "abcd", "application/octet-stream"), 201, "create partial upload");
    expect(browser.Delete("/api/offers/" + cancelled->id, post), 204, "cancel partial upload");
    expect(browser.Put(cancel_path, second, "efgh", "application/octet-stream"), 409, "cancel prevents resume");
    for (const auto& file : fs::directory_iterator(session.snapshot().received.back().path.parent_path())) {
        require(file.path().extension() != ".part", "cancel releases partial upload");
    }

    const auto folder = path / fs::u8path(u8"照片");
    fs::create_directories(folder / "empty");
    std::ofstream(folder / "notes.txt") << "packed";
    auto archive = tempshare::archive_folder(folder, [] { return false; });
    const auto zip_path = *archive.directory / "folder.zip";
    require(session.share_archive(invitation, archive.name, std::move(archive.directory)), "publish folder ZIP");
    auto shared = session.snapshot().shared;
    require(shared.size() == 1, "folder is a single download");
    const std::string download = "/api/download/" + shared.front().id;
    const auto zip = browser.Get(download, auth);
    expect(zip, 200, "download generated folder");
    require(zip->body.rfind("PK", 0) == 0 && zip->body.find("/empty/") != std::string::npos &&
            zip->body.find("/notes.txt") != std::string::npos, "ZIP includes nested files and empty directories");
    session.set_permissions({false, true, false});
    expect(browser.Get(download, auth), 404, "revoke download permission");
    session.unshare(shared.front().id);
    shared.clear();
    require(!fs::exists(zip_path), "remove the ZIP after the last reference is released");

    const auto cast_key = session.open_cast();
    require(!cast_key.empty(), "desktop authorizes cast console");
    expect(browser.Get("/api/cast/host", auth), 403, "pairing does not authorize cast host");
    auto watching = post;
    watching.emplace("X-Cast-Watch", "1");
    expect(browser.Post("/api/cast/viewer", watching, "", "text/plain"), 200, "paired browser can request viewing");
    const httplib::Headers host{{"X-TempShare", "1"}, {"X-Cast-Key", cast_key}};
    const std::string sdp = "v=0\r\nc=IN IP4 203.0.113.5\r\n"
        "a=candidate:1 1 UDP 2122260223 192.168.1.20 50000 typ host\r\n"
        "a=candidate:2 1 UDP 2122260223 203.0.113.5 50001 typ host\r\n";
    expect(browser.Post("/api/cast/offer", host, sdp, "text/plain"), 201, "publish LAN cast offer");
    const auto offer_response = browser.Post("/api/cast/viewer", watching, "", "text/plain");
    expect(offer_response, 200, "read sanitized LAN offer");
    require(offer_response->body.find("192.168.1.20") != std::string::npos &&
            offer_response->body.find("203.0.113.5") == std::string::npos, "only LAN candidates are signaled");
    auto answer = post;
    answer.emplace("X-Cast-Revision", "1");
    expect(browser.Post("/api/cast/answer", answer, sdp, "text/plain"), 201, "accept current cast answer");
    expect(browser.Post("/api/cast/answer", answer, sdp, "text/plain"), 409, "reject duplicate cast answer");
    expect(browser.Post("/api/cast/stop", host, "", "text/plain"), 204, "stop cast");
    expect(browser.Get("/api/cast/host", host), 403, "revoke cast console key");
    expect(browser.Post("/api/pair/trusted", post, "", "text/plain"), 403, "persistent device trust requires HTTPS");
    expect(browser.Delete("/api/pair", post), 204, "browser ends session");
    expect(browser.Get("/api/pair", auth), 401, "browser disconnect revokes existing session");
}

void trusted_devices(const fs::path& path) {
    const auto trust = path / "trusted";
    std::string remembered;
    {
        tempshare::Session session(path, {}, 1min, trust);
        session.start();
        auto pair = session.pair(session.snapshot().code, "Phone");
        require(pair && session.decide_pairing(pair->token, true), "approve device before trusting");
        session.set_permissions({false, false, true});
        require(session.remember_device(), "persist approved device and policy");
        remembered = session.snapshot().pairing->trusted_key;
    }
    {
        tempshare::Session session(path, {}, 1min, trust);
        session.start();
        require(!session.reconnect("unrecognized"), "reject unknown trust token");
        const auto pair = session.reconnect(remembered);
        require(pair && pair->name == "Phone" && !session.permissions().download &&
                !session.permissions().upload && session.permissions().text, "restore persisted scoped trust");
        session.set_permissions({true, false, false});
        require(session.remember_device(), "update remembered permissions");
    }
    tempshare::Session session(path, {}, 1min, trust);
    session.start();
    const auto pair = session.reconnect(remembered);
    require(pair && session.permissions().download && !session.permissions().upload &&
            !session.permissions().text, "restore the updated permissions");
    require(session.revoke_device(remembered) && !session.authorized(pair->token) &&
            !session.reconnect(remembered), "revocation ends current trust and prevents future connections");
}

void interaction_contracts(const fs::path& path) {
    tempshare::Session session(path);
    tempshare::HttpServer server(session);
    require(server.start(0, true), "start interaction server");
    httplib::Client browser("127.0.0.1", server.port());
    browser.set_read_timeout(3);
    session.start();
    const auto invite = session.snapshot().invitation;
    const auto pending = session.pair(invite, "Phone");
    require(pending && session.disconnect(pending->token), "browser cancels pending pairing");
    require(!session.pair(invite), "cancelled invitation was rotated");
    const auto pair = session.pair(session.snapshot().invitation, "Phone");
    require(pair && session.decide_pairing(pair->token, true), "approve current browser");
    const httplib::Headers auth{{"Cookie", "ts_" + std::to_string(server.port()) + "=" + pair->token}};
    const auto first = session.offer(pair->token, "first.txt", 1);
    const auto second = session.offer(pair->token, "second.txt", 1);
    const auto unseen = session.offer(pair->token, "unseen.txt", 1);
    require(first && second && unseen, "create browser file requests");
    require(session.decide_offers({first->id, second->id, first->id}, true) == 2, "approve each selected file once");
    require(session.snapshot().offers.back().state == tempshare::State::pending, "unseen request still needs approval");
    const auto status = browser.Get("/api/pair", auth);
    expect(status, 200, "poll paired browser");
    require(status->body.find("\"castReady\":false") != std::string::npos, "viewer waits for source selection");
    const auto key = session.open_cast();
    require(!key.empty(), "open local cast console");
    expect(browser.Post("/api/cast/ready", {{"X-TempShare", "1"}}, "", "text/plain"), 403,
           "only the local cast console can claim readiness");
    const httplib::Headers host{{"X-TempShare", "1"}, {"X-Cast-Key", key}};
    expect(browser.Post("/api/cast/ready", host, "", "text/plain"), 204, "authorize selected source");
    require(session.snapshot().capture_ready, "desktop reflects capture readiness");
    const auto ready = browser.Get("/api/pair", auth);
    expect(ready, 200, "poll capture readiness");
    require(ready->body.find("\"castReady\":true") != std::string::npos, "viewer can now start watching");
    const auto viewer = tempshare::random_token();
    const auto other = tempshare::random_token();
    require(session.cast_viewer(pair->token, true, viewer).has_value(), "claim viewer lease");
    require(!session.cast_viewer(pair->token, false, other), "other tab cannot stop the current viewer");
    require(!session.cast_viewer(pair->token, true, other), "other tab cannot steal the current viewer");
    require(session.cast_viewer(pair->token, false, viewer).has_value(), "owner releases viewer lease");
    require(session.cast_viewer(pair->token, true, other).has_value(), "a different tab can now watch");
    require(server.update_networks({{"192.168.20.10", 0xc0a8140a, 0xffffff00}}), "set casting subnet");
    const std::string elsewhere = "v=0\r\na=candidate:1 1 UDP 2122260223 192.168.21.10 50000 typ host\r\n";
    expect(browser.Post("/api/cast/offer", host, elsewhere, "text/plain"), 409,
           "cast ignores numeric candidates beyond the local subnet");
    const std::string same_subnet = "v=0\r\na=candidate:1 1 UDP 2122260223 192.168.20.20 50000 typ host\r\n";
    expect(browser.Post("/api/cast/offer", host, same_subnet, "text/plain"), 201,
           "cast accepts a candidate within the local subnet");
    expect(browser.Post("/api/cast/stop", host, "", "text/plain"), 204, "stop sharing");
    require(!session.snapshot().capture_ready, "stop clears capture readiness");
}

void receive_limits(const fs::path& path) {
    tempshare::Session* observed = nullptr;
    unsigned notifications = 0;
    tempshare::Session session(path, [&] {
        if (observed) observed->snapshot(tempshare::SnapshotPage::nearby);
        ++notifications;
    });
    observed = &session;
    session.start();
    require(notifications == 1, "state notification can safely obtain a snapshot");
    auto pair = session.pair(session.snapshot().invitation);
    require(pair && session.decide_pairing(pair->token, true), "approve sender");
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        const auto item = session.offer(pair->token, "archive.zip", tempshare::max_file_size);
        require(item.has_value(), "create large request without allocating its bytes");
        ids.push_back(item->id);
    }
    require(session.decide_offers(ids, true) == 4, "batch approval observes the session byte quota");
    require(session.cancel_offer(pair->token, ids.front()), "cancel releases reserved capacity");
    require(session.decide_offer(ids.back(), true), "released quota can be reused");
    const auto old_dir = path / "TempShare-abcdef012345";
    const auto workspace = old_dir / ".tempshare-partials";
    fs::create_directories(workspace);
    const auto stale = workspace / (".upload-" + std::string(32, 'a') + ".part");
    const auto fresh = workspace / (".upload-" + std::string(32, 'b') + ".part");
    const auto ordinary = old_dir / stale.filename();
    for (const auto& file : {stale, fresh, ordinary}) std::ofstream(file) << "test";
    const auto old = fs::file_time_type::clock::now() - 72h;
    fs::last_write_time(stale, old);
    fs::last_write_time(ordinary, old);
    tempshare::clean_interrupted_uploads(fs::canonical(path));
    require(!fs::exists(stale) && fs::exists(fresh) && fs::exists(ordinary),
            "reclaim only old application upload fragments");

    const auto blocked_path = path / "blocked-receive";
    std::ofstream(blocked_path) << "not a directory";
    tempshare::Session blocked(blocked_path);
    blocked.start();
    const auto client = blocked.pair(blocked.snapshot().invitation);
    require(client && blocked.decide_pairing(client->token, true), "authorize browser for disk failure");
    const auto blocked_offer = blocked.offer(client->token, "data.txt", 1);
    require(blocked_offer && blocked.decide_offer(blocked_offer->id, true), "approve upload before disk failure");
    tempshare::UploadFailure failure{};
    require(!blocked.begin_upload(client->token, blocked_offer->id, 1, {}, &failure) &&
            failure == tempshare::UploadFailure::storage && read(blocked_path) == "not a directory",
            "report a bad receive directory and preserve existing data");
}

void transfer_capacity(const fs::path& path) {
    tempshare::Session session(path);
    session.start();
    const auto pair = session.pair(session.snapshot().invitation);
    require(pair && session.decide_pairing(pair->token, true), "approve sender for capacity test");
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        const auto offer = session.offer(pair->token, "concurrent.bin", 1);
        require(offer && session.decide_offer(offer->id, true), "approve concurrent transfer");
        ids.push_back(offer->id);
    }
    tempshare::HttpServer server(session);
    require(server.start(0, true), "start capacity server");
    const httplib::Headers auth{{"Cookie", "ts_" + std::to_string(server.port()) + "=" + pair->token},
                               {"X-TempShare", "1"}};
    std::atomic<bool> release{false};
    std::vector<std::future<httplib::Result>> clients;
    struct Release {
        std::atomic<bool>& flag;
        ~Release() { flag = true; }
    } release_on_failure{release};
    for (std::size_t i = 0; i < 4; ++i) {
        clients.push_back(std::async(std::launch::async, [&, i] {
            httplib::Client client("127.0.0.1", server.port());
            client.set_read_timeout(4);
            return client.Put("/api/upload/" + ids[i], auth, 1,
                [&](std::size_t, std::size_t, httplib::DataSink& sink) {
                    while (!release) std::this_thread::sleep_for(5ms);
                    return sink.write("x", 1);
                }, "application/octet-stream");
        }));
    }
    const auto deadline = tempshare::Session::Clock::now() + 4s;
    unsigned uploading = 0;
    do {
        const auto offers = session.snapshot().offers;
        uploading = static_cast<unsigned>(std::count_if(offers.begin(), offers.end(),
            [](const tempshare::Offer& offer) { return offer.state == tempshare::State::uploading; }));
        if (uploading == 4) break;
        std::this_thread::sleep_for(10ms);
    } while (tempshare::Session::Clock::now() < deadline);
    require(uploading == 4, "four streaming requests keep their slots until completion");
    httplib::Client control("127.0.0.1", server.port());
    control.set_read_timeout(2);
    expect(control.Get("/api/pair", auth), 200, "control request proceeds during four uploads");
    const auto full = control.Put("/api/upload/" + ids.back(), auth, "y", "application/octet-stream");
    expect(full, 503, "fifth transfer receives a retryable error");
    require(full->body.find("transfer_busy") != std::string::npos, "client can identify transfer capacity error");
    release = true;
    for (auto& client : clients) expect(client.get(), 201, "admitted transfer finishes");
    expect(control.Put("/api/upload/" + ids.back(), auth, "y", "application/octet-stream"), 201,
           "slot released after completed request");
}

}  // namespace

int main() {
    try {
        Sandbox scratch;
        roundtrip(scratch.path);
        session_lifetime(scratch.path);
        network_changes(scratch.path);
        lan_features(scratch.path);
        trusted_devices(scratch.path);
        interaction_contracts(scratch.path);
        receive_limits(scratch.path);
        transfer_capacity(scratch.path);
        const tempshare::LocalNetwork lan{"192.168.70.31", 0xc0a8461f, 0xffffff00};
        require(lan.contains("192.168.70.22") && !lan.contains("192.168.71.22") &&
                !lan.contains("8.8.8.8"), "accept only the IPv4 LAN subnet");
        const tempshare::LocalNetwork wired{"192.168.74.3", 0xc0a84a03, 0xffffff00};
        require(tempshare::permits_private_lan_peer({wired}, "192.168.74.3", "192.168.70.22"),
                "allow routed clients on another private IPv4 subnet");
        require(!tempshare::permits_private_lan_peer({wired}, "192.168.12.1", "192.168.70.22") &&
                !tempshare::permits_private_lan_peer({wired}, "192.168.74.3", "8.8.8.8"),
                "reject unowned interfaces and public network clients");
        require(tempshare::is_private_ipv4("192.168.70.22") &&
                tempshare::is_private_ipv4("10.12.0.8") &&
                tempshare::is_private_ipv4("172.20.1.4") &&
                !tempshare::is_private_ipv4("169.254.10.2") &&
                !tempshare::is_private_ipv4("8.8.8.8"),
                "accept RFC1918 addresses and reject link-local and public addresses");
        require(!tempshare::permits_private_lan_peer({wired}, "192.168.74.3", "169.254.10.2"),
                "reject link-local clients without routed LAN connectivity");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
