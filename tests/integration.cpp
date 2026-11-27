#include "core/discovery.hpp"
#include "core/folder_archive.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"
#include "core/peer_link.hpp"
#include "core/peer_wire.hpp"

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

void browsable_folders(const fs::path& path) {
    const auto directory = path / fs::u8path(u8"共享资料");
    const auto other = path / "other";
    fs::create_directories(directory / "nested" / "empty");
    fs::create_directories(directory / ".tempshare-partials");
    fs::create_directories(other);
    const auto file_name = fs::u8path(u8"选这份.txt");
    std::ofstream(directory / "nested" / file_name) << "contents";
    std::ofstream(directory / "root.txt") << "root file";
    std::ofstream(other / "private.txt") << "separate share";
    std::ofstream(directory / ".tempshare-partials" / "unfinished.part") << "unfinished upload";
    std::error_code link_error;
    fs::create_directory_symlink(other, directory / "link", link_error);

    tempshare::Session session(path / "received");
    tempshare::HttpServer server(session);
    require(server.start(0, true), "start folder sharing server");
    httplib::Client browser("127.0.0.1", server.port());
    browser.set_read_timeout(2);
    session.start();
    const auto invitation = session.snapshot().invitation;
    require(session.share_folder(invitation, directory) && session.share_folder(invitation, other),
            "publish browsable folders");
    require(!session.share_folder(invitation, directory / ".tempshare-partials"),
            "do not publish the internal upload workspace");
    require(session.share(directory / "root.txt"), "ordinary files still share separately");
    const auto shared = session.snapshot().shared;
    const auto root = shared[0].id;
    const auto base = "/api/folders/" + root;
    const auto root_listing = base + "/entries/" + root;
    expect(browser.Get(root_listing), 403, "unauthenticated browser cannot list folders");
    const auto pairing = session.pair(invitation);
    require(pairing.has_value(), "request folder pairing");
    const httplib::Headers auth{{"Cookie", "ts_" + std::to_string(server.port()) + "=" + pairing->token}};
    expect(browser.Get(root_listing, auth), 403, "pending browser cannot list folders");
    require(session.decide_pairing(pairing->token, true), "approve folder viewer");
    const auto legacy = browser.Get("/api/files", auth);
    expect(legacy, 200, "list files using the original API");
    require(legacy->body.find(root) == std::string::npos && legacy->body.find(shared[2].id) != std::string::npos,
            "original API only lists directly downloadable files");
    const auto all = browser.Get("/api/files?folders=1", auth);
    expect(all, 200, "list files and folders together");
    require(all->body.find(root) != std::string::npos && all->body.find("\"folder\":true") != std::string::npos,
            "folder is marked as browsable");
    auto level = browser.Get(root_listing, auth);
    expect(level, 200, "list first level");
    require(level->body.find("nested") != std::string::npos && level->body.find("link") == std::string::npos &&
            level->body.find("private.txt") == std::string::npos && level->body.find(file_name.u8string()) == std::string::npos,
            "list immediate entries and omit linked directories");
    const auto children = session.folder_contents(pairing->token, root, root);
    require(children && children->size() == 2 && children->front().folder, "folders sort before files");
    const auto nested = children->front().id;
    level = browser.Get(base + "/entries/" + nested, auth);
    expect(level, 200, "browse nested directory");
    require(level->body.find(file_name.u8string()) != std::string::npos, "preserve Unicode file names");
    const auto nested_children = session.folder_contents(pairing->token, root, nested);
    require(nested_children && nested_children->size() == 2, "nested directory includes empty folder and file");
    const auto empty = browser.Get(base + "/entries/" + nested_children->front().id, auth);
    expect(empty, 200, "browse empty folder");
    require(empty->body == "[]", "empty folder has a valid empty listing");
    const auto child_id = nested_children->back().id;
    const auto download = base + "/download/" + child_id;
    const auto zip = browser.Get(base + "/zip/" + nested + "?file=" + child_id, auth);
    expect(zip, 200, "zip selected folder children");
    require(zip->body.rfind("PK", 0) == 0 && zip->body.find(file_name.u8string()) != std::string::npos &&
            zip->body.find("/empty/") == std::string::npos, "zip includes selected child only");
    expect(browser.Get(download), 403, "single-file download requires approval");
    expect(browser.Get("/api/folders/" + shared[1].id + "/download/" + child_id, auth), 404,
           "a descendant ID is scoped to the correct share");
    expect(browser.Get("/api/download/" + root, auth), 404, "a folder has no direct download");
    auto range = auth;
    range.emplace("Range", "bytes=0-2");
    const auto partial = browser.Get(download, range);
    expect(partial, 206, "download one file using a range");
    require(partial->body == "con", "download only the selected file");
    std::ofstream(directory / "nested" / file_name, std::ios::app) << " updated";
    expect(browser.Get(download, auth), 409, "changed descendant requires sharing again");
    expect(browser.Head(download, auth), 409, "HEAD exposes an unavailable download before navigating");
    require(session.share_folder(invitation, directory, root), "refresh an existing folder");
    const auto refreshed = session.folder_contents(pairing->token, root, nested);
    require(refreshed && refreshed->back().id != child_id, "refresh keeps the directory but replaces the changed file ID");
    expect(browser.Get(download, auth), 404, "refresh revokes the old file link");
    const auto new_download = base + "/download/" + refreshed->back().id;
    expect(browser.Get(new_download + "?preview=1", auth), 200, "preview a text file after refresh");
    session.set_permissions({false, true, true});
    expect(browser.Get(root_listing, auth), 404, "disabling downloads hides folder contents");
    expect(browser.Get(download, auth), 404, "disabling downloads revokes file access");
    session.set_permissions({true, true, true});
    session.unshare(root);
    expect(browser.Get(root_listing, auth), 404, "removing a folder hides its listing");
    expect(browser.Get(download, auth), 404, "removing a folder revokes existing links");
}

void upload_directories_and_history(const fs::path& path) {
    const auto inbox = path / "history-inbox";
    const auto history = path / "history-config" / "received-history";
    fs::path received;
    {
        tempshare::Session session(inbox, {}, 30min, {}, history);
        tempshare::HttpServer server(session);
        require(server.start(0, true), "start nested upload server");
        httplib::Client browser("127.0.0.1", server.port());
        session.start();
        const auto pairing = session.pair(session.snapshot().invitation, "Test Phone");
        require(pairing && session.decide_pairing(pairing->token, true), "approve nested upload browser");
        const httplib::Headers auth{{"Cookie", "ts_" + std::to_string(server.port()) + "=" + pairing->token},
                                   {"X-TempShare", "1"}};
        auto invalid = auth;
        invalid.emplace("X-File-Name", "photo.txt");
        invalid.emplace("X-File-Size", "5");
        invalid.emplace("X-Upload-Directory", "%2e%2e%2foutside");
        expect(browser.Post("/api/offers", invalid, "", "text/plain"), 409, "reject relative path traversal");
        auto headers = auth;
        headers.emplace("X-File-Name", "photo.txt");
        headers.emplace("X-File-Size", "5");
        headers.emplace("X-Upload-Directory", "album%2ftrip");
        headers.emplace("X-Upload-Batch", std::string(32, 'a'));
        expect(browser.Post("/api/offers", headers, "", "text/plain"), 201, "register folder entry");
        const auto offer = session.snapshot().offers.back();
        require(offer.batch == std::string(32, 'a') && session.decide_offer(offer.id, true), "approve file in batch");
        expect(browser.Put("/api/upload/" + offer.id, auth, "hello", "application/octet-stream"), 201, "receive nested file");
        const auto file = session.snapshot().received.back();
        received = file.path;
        require(received.parent_path().filename() == "trip" && received.parent_path().parent_path().filename() == "album",
                "selected directory structure is preserved");
        require(file.received_from == "Test Phone" && file.received_at > 0, "record sender and receive time");
    }
    tempshare::Session restored(inbox, {}, 30min, {}, history);
    require(restored.snapshot().received.size() == 1 && restored.snapshot().received.front().path == received,
            "receive history survives restart");
    require(restored.clear_received() && fs::is_regular_file(received), "clearing history preserves files");
    tempshare::Session cleared(inbox, {}, 30min, {}, history);
    require(cleared.snapshot().received.empty(), "cleared history remains empty");
}

void independent_lan_connections(const fs::path& path) {
    tempshare::Session first(path / "first-device");
    tempshare::Session second(path / "second-device");
    tempshare::HttpServer first_server(first);
    tempshare::HttpServer second_server(second);
    require(first_server.start(0, true) && second_server.start(0, true) && first_server.port() != second_server.port(),
            "each LAN connection has an independent listener");
    first.start();
    second.start();
    httplib::Client a("127.0.0.1", first_server.port());
    httplib::Client b("127.0.0.1", second_server.port());
    const auto one = first.pair(first.snapshot().invitation, "First device");
    const auto two = second.pair(second.snapshot().invitation, "Second device");
    require(one && two && first.decide_pairing(one->token, true) && second.decide_pairing(two->token, true),
            "approve both devices");
    const httplib::Headers a_cookie{{"Cookie", "ts_" + std::to_string(first_server.port()) + "=" + one->token}};
    const httplib::Headers b_cookie{{"Cookie", "ts_" + std::to_string(second_server.port()) + "=" + two->token}};
    expect(a.Get("/api/pair", a_cookie), 200, "first device is paired");
    expect(b.Get("/api/pair", b_cookie), 200, "second device is paired");
    expect(a.Get("/api/files", b_cookie), 403, "second device cannot read the first connection");
    const auto file = path / "devices.txt";
    std::ofstream(file) << "second connection only";
    require(second.share(file), "share only with the second device");
    const auto shared = second.snapshot().shared.back().id;
    expect(a.Get("/api/download/" + shared, a_cookie), 404, "shared IDs stay in their own connection");
    const auto available = b.Get("/api/download/" + shared, b_cookie);
    expect(available, 200, "second device gets its own shared file");
    require(available->body == "second connection only", "second connection file bytes are correct");
    first.end();
    expect(a.Get("/api/pair", a_cookie), 401, "first device disconnects independently");
    expect(b.Get("/api/download/" + shared, b_cookie), 200, "second device continues downloading");
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
    require(session.notification_state().incoming_messages == 0, "local text does not notify the desktop");
    expect(browser.Post("/api/text", post, u8"中文\ncode()", "text/plain"), 201, "browser sends multiline text");
    require(session.notification_state().incoming_messages == 1, "browser text signals a desktop notification");
    const auto entries = browser.Get("/api/text", auth);
    expect(entries, 200, "read text history");
    require(entries->body.find("code()") != std::string::npos && session.snapshot().messages.size() == 2,
            "both directions share the same history");
    expect(browser.Post("/api/text", post, std::string(tempshare::max_text_size + 1, 'x'), "text/plain"),
           400, "bound text request size");
    session.set_permissions({true, true, false});
    expect(browser.Get("/api/text", auth), 403, "revoke text permission");
    require(!session.snapshot().messages.empty(), "revoking text preserves history until explicitly cleared");

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
    expect(browser.Post("/api/cast/viewer", post, "", "text/plain"), 200, "release the direct viewer");
    const std::string viewer(32, 'a');
    auto frame_viewer = watching;
    frame_viewer.emplace("X-Cast-Viewer", viewer);
    frame_viewer.emplace("X-Cast-Frames", "1");
    const auto fallback = browser.Post("/api/cast/viewer", frame_viewer, "", "text/plain");
    expect(fallback, 200, "negotiate compatibility playback");
    require(fallback->body.find("\"frames\":true") != std::string::npos, "compatibility playback enabled");
    const auto playback = session.cast_host(cast_key);
    require(playback && playback->frames && playback->offer.empty(), "desktop sees the compatibility viewer");
    auto publish = host;
    publish.emplace("X-Cast-Revision", std::to_string(playback->revision));
    const std::string jpeg = "\xff\xd8" "test frame" "\xff\xd9";
    expect(browser.Post("/api/cast/frame", publish, "invalid", "image/jpeg"), 400, "reject malformed frame");
    expect(browser.Post("/api/cast/frame", publish, std::string(tempshare::max_cast_frame_size + 1, 'x'), "image/jpeg"),
           400, "reject oversized frame");
    expect(browser.Post("/api/cast/frame", frame_viewer, jpeg, "image/jpeg"), 403, "viewer cannot publish");
    expect(browser.Post("/api/cast/frame", publish, jpeg, "image/jpeg"), 204, "desktop publishes a frame");
    expect(browser.Get("/api/cast/frame", auth), 403, "another tab cannot read frames");
    auto read_frame = auth;
    read_frame.emplace("X-Cast-Viewer", viewer);
    const auto frame = browser.Get("/api/cast/frame", read_frame);
    expect(frame, 200, "active viewer reads the latest frame");
    require(frame->body == jpeg, "frame bytes unchanged");
    const auto sequence = frame->get_header_value("X-Cast-Sequence");
    require(!sequence.empty(), "frame has a sequence");
    const auto previous_sequence = std::stoull(sequence);
    for (unsigned index = 0; index < 4; ++index) {
        std::this_thread::sleep_for(12ms);
        require(session.publish_cast_frame(cast_key, playback->revision, jpeg), "publish high-rate compatibility frame");
    }
    const auto high_rate = session.cast_frame(pair->token, viewer, previous_sequence);
    require(high_rate && high_rate->sequence == previous_sequence + 4, "server no longer discards frames at a 32ms ceiling");
    std::string detailed_jpeg(512 * 1024, 'x');
    detailed_jpeg[0] = '\xff'; detailed_jpeg[1] = '\xd8';
    detailed_jpeg[detailed_jpeg.size() - 2] = '\xff'; detailed_jpeg.back() = '\xd9';
    std::this_thread::sleep_for(12ms);
    expect(browser.Post("/api/cast/frame", publish, detailed_jpeg, "image/jpeg"), 204, "accept a detailed frame above the old 384KiB limit");
    const auto latest = browser.Get("/api/cast/frame", read_frame);
    expect(latest, 200, "read latest high-rate frame");
    read_frame.emplace("X-Cast-After", latest->get_header_value("X-Cast-Sequence"));
    auto wait_frame = read_frame;
    wait_frame.emplace("X-Cast-Wait", "1");
    auto next_frame = std::async(std::launch::async, [&] {
        httplib::Client waiting("127.0.0.1", server.port());
        return waiting.Get("/api/cast/frame", wait_frame);
    });
    std::this_thread::sleep_for(40ms);
    expect(browser.Post("/api/cast/frame", publish, jpeg, "image/jpeg"), 204, "publish the waiting viewer's next frame");
    const auto next = next_frame.get();
    expect(next, 200, "a waiting viewer receives the next frame");
    require(next->get_header_value("X-Cast-Sequence") != sequence, "push only a new frame");
    read_frame.erase("X-Cast-After");
    read_frame.emplace("X-Cast-After", next->get_header_value("X-Cast-Sequence"));
    expect(browser.Get("/api/cast/frame", read_frame), 204, "skip previously displayed frame");
    expect(browser.Post("/api/cast/stop", host, "", "text/plain"), 204, "stop cast");
    expect(browser.Get("/api/cast/frame", read_frame), 403, "stop revokes the last frame");
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
    tempshare::Session other_slot(path / "other-slot", {}, 1min, trust);
    other_slot.start();
    const auto other_pair = other_slot.reconnect(remembered);
    require(other_pair.has_value(), "device trust is reusable across session slots");
    require(session.revoke_device(remembered) && !session.authorized(pair->token) &&
            !session.reconnect(remembered), "revocation ends current trust and prevents future connections");
    other_slot.expire();
    require(!other_slot.authorized(other_pair->token) && !other_slot.reconnect(remembered),
            "revocation is observed by another active slot and cannot be restored from stale cache");
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
    expect(browser.Post("/api/cast/offer", host, elsewhere, "text/plain"), 201,
           "cast accepts candidates across routed private subnets");
    const std::string public_candidate = "v=0\r\na=candidate:1 1 UDP 2122260223 203.0.113.10 50000 typ host\r\n";
    expect(browser.Post("/api/cast/offer", host, public_candidate, "text/plain"), 409,
           "cast rejects a public candidate");
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

template<typename Predicate>
void eventually(Predicate predicate, const char* description) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    require(predicate(), description);
}

void native_desktop_connections(const fs::path& path) {
    tempshare::Session a(path / "native-a"), b(path / "native-b");
    tempshare::HttpServer host_a(a), host_b(b);
    require(host_a.start(0, true) && host_b.start(0, true), "start both native desktop listeners");
    a.start(); b.start();
    host_b.peer().set_identity(std::string(32, 'b'), "Test peer B");
    require(!host_a.peer().join(host_b.url("127.0.0.1"), {"https://127.0.0.1:" + std::to_string(host_b.port())}),
            "alternate addresses cannot change transport security");
    const auto started = host_a.peer().join("http://localhost:" + std::to_string(host_b.port()), {host_b.url("127.0.0.1")}, std::string(32, 'b'));
    if (!started) std::cerr << "native join diagnostic: port=" << host_b.port() << " active=" << a.is_active()
        << " paired=" << a.snapshot().pairing.has_value() << " attached=" << host_a.peer().snapshot().attached
        << " hostname=" << tempshare::parse_lan_endpoint("http://localhost:" + std::to_string(host_b.port()), true).has_value()
        << " loopback=" << host_b.url("127.0.0.1") << '\n';
    require(started,
            "join remote through an available alternate without asking users to select an address");
    eventually([&] { return b.snapshot().pairing.has_value(); }, "remote desktop receives approval request");
    require(!host_a.peer().snapshot().approved && b.snapshot().pairing->state == tempshare::State::pending,
            "desktop discovery never auto-approves remote access");
    require(a.decide_pairing(a.snapshot().pairing->token, true), "simulate remote peer claiming its own approval");
    std::this_thread::sleep_for(900ms);
    require(b.snapshot().pairing->state == tempshare::State::pending,
            "remote approval cannot bypass the receiving desktop's explicit consent");
    require(b.decide_pairing(b.snapshot().pairing->token, true), "approve remote desktop");
    eventually([&] { return host_a.peer().snapshot().approved && host_b.peer().snapshot().approved; },
               "both native desktops track the same approved link");
    require(a.snapshot().pairing && b.snapshot().pairing, "both desktop session views contain the peer");
    require(!b.receive_peer_message("wrong-token", {"injected", true}), "stale or foreign link cannot inject native messages");
    require(a.send_text(u8"A 发往 B\nhello") && b.send_text(u8"B 发往 A\nreply"), "both desktops send messages");
    eventually([&] { return a.snapshot().messages.size() == 2 && b.snapshot().messages.size() == 2; },
               "native messages synchronize in both directions without echo loops");
    const auto key_a = a.open_cast();
    require(!key_a.empty() && a.cast_ready(key_a), "joining desktop can publish its own screen");
    eventually([&] { return host_b.peer().snapshot().capture_ready; }, "original host can watch joining desktop");
    const auto key_b = b.open_cast();
    require(!key_b.empty() && b.cast_ready(key_b), "original host can also publish its screen");
    eventually([&] { return host_a.peer().snapshot().capture_ready; }, "both screen directions are available");
    const auto file = path / "native-shared.txt";
    std::ofstream(file) << "native file contents";
    require(b.share(file), "native peer publishes a file");
    eventually([&] { return host_a.peer().snapshot().files.size() == 1; }, "remote file list appears in native UI");
    require(host_a.peer().download(host_a.peer().snapshot().files.front()), "native peer downloads selected file");
    eventually([&] { return !host_a.peer().snapshot().transferring; }, "native download completes");
    require(a.snapshot().received.size() == 1 && read(a.snapshot().received.front().path) == "native file contents",
            "native download is saved and recorded");
    httplib::Client browser("127.0.0.1", host_a.port());
    expect(browser.Get("/peer/api/pair"), 403, "local bridge still requires its browser authorization");
    const auto secret = host_a.peer().browser_key();
    expect(browser.Post("/peer/authorize", {{"X-TempShare", "1"}, {"X-Peer-Key", secret}}, "", "text/plain"),
           204, "authorize native peer viewing on loopback");
    const httplib::Headers bridge{{"Cookie", "ts_peer_" + std::to_string(host_a.port()) + "=" + secret}};
    expect(browser.Get("/peer/api/pair", bridge), 200, "local viewer reaches the remote session through native credentials");
    a.clear_messages();
    eventually([&] { return b.snapshot().messages.empty(); }, "explicit clear synchronizes to the peer");
    host_a.peer().leave();
    eventually([&] { return !host_b.peer().snapshot().attached; }, "both peers observe leave");
    require(a.is_active() && b.is_active() && b.snapshot().shared.size() == 1,
            "leaving keeps the session and published content");
}

void gateway_connections(const fs::path& path) {
    tempshare::Session primary(path / "gateway-1"), secondary(path / "gateway-2"), guest(path / "gateway-guest");
    tempshare::HttpServer public_server(primary), private_server(secondary), guest_server(guest);
    require(public_server.start(0, true) && private_server.start(0, true) && guest_server.start(0, true),
            "start native gateway test listeners");
    public_server.configure_gateway({&public_server, &private_server});
    primary.start(); secondary.start(); guest.start();
    const auto occupied = primary.pair(primary.snapshot().invitation, "Already connected");
    require(occupied && primary.decide_pairing(occupied->token, true), "occupy primary session");
    httplib::Client browser("127.0.0.1", public_server.port());
    const auto root = browser.Get("/");
    expect(root, 302, "new browser is routed to an available session");
    require(root->get_header_value("Location") == "/s/2/", "gateway chooses available session route");
    expect(browser.Get("/s/2/"), 200, "secondary page uses public entry");
    expect(browser.Get("/s/2/api/cast/host"), 403, "gateway does not expose capture authorization");
    require(guest_server.peer().join(public_server.url("127.0.0.1")), "join a busy device through its single entry");
    eventually([&] { return secondary.snapshot().pairing.has_value(); }, "gateway forwards to free native session");
    require(secondary.decide_pairing(secondary.snapshot().pairing->token, true), "approve native guest on second session");
    eventually([&] { return guest_server.peer().snapshot().approved && private_server.peer().snapshot().approved; },
               "bidirectional native state works through a gateway route");
    require(guest.send_text("gateway native message"), "send message through shared entry");
    eventually([&] { return !secondary.snapshot().messages.empty(); }, "gateway message reaches only its session");
    require(primary.snapshot().messages.empty(), "gateway preserves session isolation");
    const auto outbound = path / "native-send.txt";
    std::ofstream(outbound) << "send and save";
    require(guest_server.peer().send(outbound), "native sender requests direct delivery");
    eventually([&] { return !secondary.snapshot().offers.empty(); }, "receiver sees native file request");
    require(secondary.decide_offer(secondary.snapshot().offers.front().id, true), "receiver explicitly approves native delivery");
    eventually([&] { return !guest_server.peer().snapshot().transferring; }, "native send completes");
    if (secondary.snapshot().received.empty()) std::cerr << guest_server.peer().snapshot().transfer_status << '\n';
    require(secondary.snapshot().received.size() == 1 &&
            read(secondary.snapshot().received.front().path) == "send and save", "native upload saves exact bytes");
    require(guest_server.peer().send(outbound), "request a second native delivery before leaving");
    eventually([&] { return secondary.snapshot().offers.size() == 2; }, "second delivery waits for approval");
    guest_server.peer().leave();
    eventually([&] { return !private_server.peer().snapshot().attached; }, "leave routed native connection");
    eventually([&] { return !guest_server.peer().snapshot().transferring; }, "leaving cancels the pending native send");
    require(secondary.snapshot().received.size() == 1 && secondary.snapshot().offers.back().state == tempshare::State::rejected,
            "a departed sender cannot complete or retain an approved upload");
    public_server.stop();
    private_server.stop();
    guest_server.stop();
}


void peer_protocol_contracts() {
    const tempshare::LocalNetwork local{"192.168.1.10", 0xc0a8010a, 0xffffff00};
    tempshare::Peer peer{"192.168.1.10", 9999, "Same PC other instance", "Windows", false, std::string(32, 'b'), {}};
    require(tempshare::is_local_peer(peer, {local}, std::string(32, 'a')), "hide local address even with a different port and install identity");
    peer.id.clear();
    require(tempshare::is_local_peer(peer, {local}, ""), "hide old protocol self announcements without a device ID");
    peer.address = "192.168.1.20"; peer.id = std::string(32, 'a');
    require(tempshare::is_local_peer(peer, {local}, peer.id), "hide same install identity after address changes");
    peer.id = std::string(32, 'b');
    require(!tempshare::is_local_peer(peer, {local}, std::string(32, 'a')), "keep another physical device with the same hostname visible");
    const auto endpoint = tempshare::parse_lan_endpoint(" http://192.168.1.2:8787/s/2/#0123456789abcdef0123456789abcdef ");
    require(endpoint && endpoint->route == "/s/2" && endpoint->invitation.size() == 32, "parse full LAN invitation links");
    require(tempshare::parse_lan_endpoint("http://office-pc.local:8787").has_value(), "allow LAN hostnames for background private-address resolution");
    require(!tempshare::parse_lan_endpoint("https://8.8.8.8:443") &&
            !tempshare::parse_lan_endpoint("http://192.168.1.2:0") &&
            !tempshare::parse_lan_endpoint("http://user@192.168.1.2") &&
            !tempshare::parse_lan_endpoint("http://192.168.1.2/s/9"), "reject unsafe or malformed peer endpoints");
    const std::vector<std::string> fields{"", u8"中文\n消息", std::string("a\0b", 3)};
    const auto encoded = tempshare::peer_wire::encode(fields);
    require(tempshare::peer_wire::decode(encoded) == fields, "binary peer fields preserve multiline UTF-8 and null bytes");
    require(!tempshare::peer_wire::decode(encoded.substr(0, encoded.size() - 1)) &&
            !tempshare::peer_wire::decode(std::string(4, '\xff')), "reject truncated and oversized peer fields");
}

void revised_session_lifecycle(const fs::path& path) {
    tempshare::Session session(path, {}, 60ms);
    session.start();
    const auto pair = session.pair(session.snapshot().invitation, "Lifecycle peer");
    require(pair && session.decide_pairing(pair->token, true), "approve lifecycle peer");
    require(session.send_text("keep this message"), "create retained message");
    session.set_permissions({true, true, false, true});
    require(session.snapshot().messages.size() == 1 && !session.messages(pair->token), "permission hides messages without deleting them");
    session.set_permissions({true, true, true, true});
    require(session.messages(pair->token)->size() == 1, "restoring text permission restores access to history");
    session.download_progress("active-download", "file.txt", 100, 10);
    std::this_thread::sleep_for(80ms);
    require(session.authorized(pair->token), "an active progressing task receives expiry grace");
    session.download_finished("active-download", true);
    require(!session.is_active(), "grace ends after the task completes");
    session.download_progress("late-download", "late.txt", 100, 1);
    require(!session.begin_upload("wrong-token", "missing", 1) && !session.uploading("wrong-token", "missing") &&
            !session.downloading(pair->token, "missing") && !session.is_active(),
            "late or unauthorized transfer probes never revive an expired session");
    session.start();
    const auto again = session.pair(session.snapshot().invitation, "screen peer");
    require(again && session.decide_pairing(again->token, true), "approve screen peer");
    session.set_permissions({true, true, true, false});
    require(session.open_cast().empty(), "screen permission gates native capture authorization");
    session.set_permissions({true, true, true, true});
    const auto host = session.open_cast();
    require(!host.empty() && session.cast_ready(host), "screen permission enables capture");
    session.set_permissions({true, true, true, false});
    require(!session.cast_host(host), "revoking screen permission invalidates capture keys");
}

void configurable_session_lifetime(const fs::path& path) {
    tempshare::Session session(path / "long-running", {}, 40ms);
    require(session.set_lifetime(0ms) && !session.is_active(), "duration changes cannot revive a stopped session");
    session.start();
    auto state = session.snapshot();
    const auto pair = session.pair(state.code, "Home device");
    require(pair && session.decide_pairing(pair->token, true), "unlimited sessions still require explicit pairing");
    std::this_thread::sleep_for(60ms);
    require(session.authorized(pair->token) && !session.expire() && session.snapshot().unlimited,
            "unlimited session stays authorized beyond the original deadline");
    require(!session.set_lifetime(-1ms) && !session.set_lifetime(std::chrono::hours(24 * 31)), "reject invalid session durations");
    require(session.set_lifetime(40ms) && !session.snapshot().unlimited, "finite duration applies without unpairing");
    require(session.send_text("retained"), "changing duration preserves messages and authorization");
    std::this_thread::sleep_for(60ms);
    require(!session.authorized(pair->token) && session.expire(), "finite sessions retain expiry behavior");
    require(session.set_lifetime(0ms) && !session.is_active(), "unlimited preference does not revive expired authorization");
    session.start(); session.end();
    require(!session.is_active() && !session.authorized(pair->token), "explicit end stops an unlimited session");
}

}  // namespace

int main() {
    try {
        Sandbox scratch;
        peer_protocol_contracts();
        revised_session_lifecycle(scratch.path);
        configurable_session_lifetime(scratch.path);
        native_desktop_connections(scratch.path);
        gateway_connections(scratch.path);
        roundtrip(scratch.path);
        browsable_folders(scratch.path);
        upload_directories_and_history(scratch.path);
        independent_lan_connections(scratch.path);
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
