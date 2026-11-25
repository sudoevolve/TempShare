#include "core/discovery.hpp"
#include "core/folder_archive.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"
#include "platform.hpp"

#define AINO_IMPLEMENTATION
#include "aino.hpp"
#ifdef _WIN32
#include <shellapi.h>
#endif

#include <qrcodegen.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

using aino::Color;
using aino::Rect;
using aino::Style;
using aino::Ui;
using tempshare::State;

const Color background{246, 247, 249};
const Color sidebar{237, 239, 242};
const Color surface{255, 255, 255};
const Color border{220, 223, 228};
const Color text{30, 32, 36};
const Color muted{103, 108, 117};
const Color accent{0, 122, 255};
const Color online{45, 145, 100};

struct Options {
    unsigned short port = 8787;
    std::filesystem::path directory;
    std::string cert;
    std::string key;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i += 2) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + arg);
        const std::string value = argv[i + 1];
        if (arg == "--receive-dir") options.directory = std::filesystem::absolute(std::filesystem::u8path(value));
        else if (arg == "--cert") options.cert = value;
        else if (arg == "--key") options.key = value;
        else if (arg == "--port") {
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, options.port);
            if (parsed.ec != std::errc() || parsed.ptr != end || !options.port) {
                throw std::invalid_argument("Invalid port");
            }
        } else {
            throw std::invalid_argument("Unknown option " + arg);
        }
    }
    if (options.cert.empty() != options.key.empty()) throw std::invalid_argument("Use --cert and --key together");
    return options;
}

void label(Ui& ui, const std::string& value, Rect rect, float size = 14,
           const Color& color = text, bool bold = false) {
    auto style = Style().font(size).left();
    if (bold) style.semi_bold();
    ui.draw_text(value, rect, color, style);
}

bool button(Ui& ui, const std::string& value, Rect rect, bool primary = false, std::uint64_t id = 0) {
    auto style = Style().font(13).corner(8).id(id);
    if (primary) style.primary();
    else style.secondary();
    return ui.button(value, rect, style);
}

std::string readable_size(std::uint64_t size) {
    constexpr double mib = 1024 * 1024;
    if (size < 1024) return std::to_string(size) + " B";
    std::ostringstream output;
    output << std::fixed << std::setprecision(1);
    if (size < mib) output << size / 1024.0 << " KiB";
    else output << size / mib << " MiB";
    return output.str();
}

void card(Ui& ui, Rect box) {
    ui.fill_rect(box, surface, 8);
    ui.stroke_rect(box, border, 8);
}

void draw_qr(Ui& ui, const qrcodegen::QrCode& qr, Rect area) {
    const float unit = std::floor(std::min(area.w, area.h) / (qr.getSize() + 8));
    const float size = unit * (qr.getSize() + 8);
    const float x = std::floor(area.x + (area.w - size) / 2);
    const float y = std::floor(area.y + (area.h - size) / 2);
    ui.fill_rect({x, y, size, size}, Color{255, 255, 255});
    for (int row = 0; row < qr.getSize(); ++row) {
        for (int column = 0; column < qr.getSize(); ++column) {
            if (qr.getModule(column, row)) {
                ui.fill_rect({x + (column + 4) * unit, y + (row + 4) * unit, unit, unit}, Color{0, 0, 0});
            }
        }
    }
}

struct View {
    using Page = tempshare::SnapshotPage;
    Page page = Page::share;
    std::string selected_address;
    std::size_t peer_page = 0;
    std::size_t offer_page = 0;
    std::string status;
    std::string path;
    std::string folder;
    std::string draft;
    std::string previous_invitation;
    std::size_t message_page = 0;
    std::size_t device_page = 0;
    std::future<std::string> archive;
    std::thread archive_worker;
    std::string last_qr_url;
    std::optional<qrcodegen::QrCode> qr;
};

std::vector<std::filesystem::path> pick_paths(View& view, bool directory) {
    bool failed = false;
    auto paths = choose_paths(directory, &failed);
    if (failed) view.status = "无法打开文件选择器，可在设置中输入路径";
    return paths;
}

std::size_t page_controls(Ui& ui, std::size_t& page, std::size_t total, std::size_t per_page,
                          float x, float y, float width) {
    const std::size_t pages = total ? (total - 1) / per_page + 1 : 1;
    page = std::min(page, pages - 1);
    if (pages == 1) return 0;
    label(ui, std::to_string(page + 1) + " / " + std::to_string(pages), {x + width - 116, y, 46, 28},
          12, muted);
    if (page > 0 && button(ui, "‹", {x + width - 68, y, 30, 28})) --page;
    if (page + 1 < pages && button(ui, "›", {x + width - 32, y, 30, 28})) ++page;
    return page * per_page;
}

void share_paths(tempshare::Session& session, View& view, const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    std::size_t added = 0;
    for (const auto& path : paths) {
        if (session.share(std::filesystem::absolute(path))) ++added;
    }
    view.status = "已添加 " + std::to_string(added) + " 个文件";
    if (added < paths.size()) view.status += "，部分文件无法读取、超过 1 GiB 或列表已满";
    view.page = View::Page::share;
}

void share_folder(tempshare::Session& session, aino::Window& window, View& view,
                  const tempshare::Snapshot& snapshot) {
    if (view.archive.valid()) return;
    const auto paths = pick_paths(view, true);
    if (paths.empty()) return;
    std::packaged_task<std::string()> archive([&session, folder = paths.front(), invitation = snapshot.invitation] {
        try {
            auto result = tempshare::archive_folder(folder, [&] { return !session.invitation_active(invitation); });
            return session.share_archive(invitation, std::move(result.name), std::move(result.directory))
                ? std::string("文件夹已打包并加入分享") : std::string("会话已结束或分享列表已满");
        } catch (const std::exception& error) {
            return std::string("文件夹打包失败 · ") + error.what();
        }
    });
    view.archive = archive.get_future();
    view.archive_worker = std::thread([&window, task = std::move(archive)]() mutable {
        task();
        window.wake();
    });
    view.status = "正在打包文件夹…";
}

void render_connection(Ui& ui, aino::Window& window, tempshare::Session& session, const tempshare::HttpServer& server,
                       const tempshare::Snapshot& snapshot, const std::vector<tempshare::LocalNetwork>& networks,
                       View& view, float qr_left) {
    card(ui, {qr_left, 106, 256, 430});
    label(ui, "连接设备", {qr_left + 18, 121, 220, 30}, 16, text, true);
    if (snapshot.active) {
        const auto selected = std::find_if(networks.begin(), networks.end(), [&](const tempshare::LocalNetwork& network) {
            return network.address == view.selected_address;
        });
        const auto current = selected == networks.end() ? networks.begin() : selected;
        const auto address = networks.empty() ? "127.0.0.1" : current->address;
        const auto url = server.url(address);
        const auto invitation = url + '#' + snapshot.invitation;
        if (view.last_qr_url != invitation) {
            view.qr = qrcodegen::QrCode::encodeText(invitation.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            view.last_qr_url = invitation;
        }
        if (snapshot.pairing) {
            const auto& pairing = *snapshot.pairing;
            label(ui, pairing.name, {qr_left + 18, 162, 220, 32}, 17, text, true);
            if (pairing.state == State::pending) {
                label(ui, "请求连接 · 核对浏览器上的验证码", {qr_left + 18, 201, 220, 24}, 12, muted);
                label(ui, pairing.verification, {qr_left + 18, 233, 220, 42}, 27, online, true);
                if (button(ui, "允许连接", {qr_left + 18, 287, 104, 34}, true)) {
                    session.decide_pairing(pairing.token, true);
                }
                if (button(ui, "拒绝", {qr_left + 134, 287, 104, 34})) session.decide_pairing(pairing.token, false);
            } else {
                label(ui, "已连接", {qr_left + 18, 200, 220, 26}, 13, online, true);
                const auto waiting = std::count_if(snapshot.offers.begin(), snapshot.offers.end(),
                    [](const tempshare::Offer& offer) { return offer.state == State::pending; });
                if (waiting && button(ui, std::to_string(waiting) + " 个文件待接收",
                                      {qr_left + 18, 249, 220, 36}, true)) {
                    view.page = View::Page::share;
                    view.offer_page = 0;
                    window.wake();
                }
                if (snapshot.capture_ready) {
                    label(ui, "正在共享屏幕", {qr_left + 18, 311, 220, 27}, 13, online);
                    if (button(ui, "停止共享", {qr_left + 18, 342, 220, 33})) session.stop_cast();
                }
            }
        } else {
            if (networks.empty()) {
                label(ui, "没有可用的局域网地址", {qr_left + 18, 165, 220, 32}, 15, text, true);
                label(ui, "请连接与手机相同的网络", {qr_left + 18, 201, 220, 25}, 12, muted);
            } else {
                draw_qr(ui, *view.qr, {qr_left + 25, 151, 206, 206});
            }
            if (!networks.empty()) {
                label(ui, url, {qr_left + 18, 361, 220, 24}, 12, muted);
                const auto& network = *current;
                const auto kind = network.interface_name.empty() ? std::string{} :
                    network.interface_name + (network.wireless ? " · Wi-Fi" : " · 以太网");
                label(ui, kind.empty() ? "手机需可访问此地址" : kind + " · 手机需可访问",
                      {qr_left + 18, 385, 220, 19}, 11, muted);
                label(ui, "连接码  " + snapshot.code, {qr_left + 18, 402, 220, 26}, 16, text, true);
            }
            if (!networks.empty() && button(ui, "复制链接", {qr_left + 18, 430, 102, 32})) {
                view.status = window.try_set_clipboard(invitation) ? "邀请链接已复制" : "无法写入剪贴板";
            }
            if (networks.size() > 1 && button(ui, "切换地址", {qr_left + 132, 430, 106, 32})) {
                view.selected_address = std::next(current) == networks.end() ?
                    networks.front().address : std::next(current)->address;
                window.wake();
            }
        }
        if (button(ui, "结束会话", {qr_left + 18, 484, 220, 34})) {
            session.end();
            view.status = "会话已结束";
        }
    } else {
        label(ui, "当前没有会话", {qr_left + 18, 170, 220, 30}, 13, muted);
        if (button(ui, "新建会话", {qr_left + 18, 217, 220, 38}, true)) {
            session.start();
            view.status.clear();
        }
    }
}

void render_share(Ui& ui, aino::Window& window, tempshare::Session& session,
                  const tempshare::Snapshot& snapshot, View& view, float x, float transfer_width, float bottom) {
    card(ui, {x, 106, transfer_width, 120});
    label(ui, "发送文件", {x + 18, 119, transfer_width - 36, 30}, 16, text, true);
#ifdef _WIN32
    label(ui, "拖入文件，或选择要分享的文件", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#else
    label(ui, "选择文件，或在设置中输入文件路径", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#endif
    if (snapshot.active && button(ui, "选择文件", {x + 18, 181, 114, 32}, true)) {
        share_paths(session, view, pick_paths(view, false));
    }
    if (snapshot.active && !view.archive.valid() && button(ui, "分享文件夹", {x + 141, 181, 115, 32})) {
        share_folder(session, window, view, snapshot);
    }
    constexpr float list_y = 245.0f;
    label(ui, "接收请求", {x, list_y, 104, 26}, 16, text, true);
    std::vector<const tempshare::Offer*> pending;
    for (const auto& offer : snapshot.offers) {
        if (offer.state == State::pending || offer.state == State::approved || offer.state == State::uploading) {
            pending.push_back(&offer);
        }
    }
    std::stable_partition(pending.begin(), pending.end(), [](const tempshare::Offer* offer) {
        return offer->state == State::pending;
    });
    const auto offer_start = page_controls(ui, view.offer_page, pending.size(), 3, x, list_y, transfer_width);
    std::vector<std::string> visible;
    std::uint64_t visible_bytes = 0;
    for (std::size_t i = offer_start; i < std::min(offer_start + 3, pending.size()); ++i) {
        if (pending[i]->state == State::pending) {
            visible.push_back(pending[i]->id);
            visible_bytes += pending[i]->size;
        }
    }
    if (visible.size() > 1 && button(ui, "接收本页 " + std::to_string(visible.size()) + " 个",
                                     {x + 104, list_y, 92, 28}, true)) {
        const auto count = session.decide_offers(visible, true);
        view.status = "已接受 " + std::to_string(count) + " 个文件";
        if (count < visible.size()) view.status += "，部分请求已变化或超出会话接收上限";
    }
    if (visible.size() > 1 && transfer_width >= 400) {
        label(ui, readable_size(visible_bytes), {x + 205, list_y, 76, 28}, 12, muted);
    }
    float row = list_y + 34;
    if (pending.empty()) {
        label(ui, "暂无接收请求", {x, row, transfer_width, 26}, 13, muted);
        row += 36;
    } else {
        for (std::size_t i = offer_start; i < std::min(offer_start + 3, pending.size()); ++i, row += 48) {
            const auto& offer = *pending[i];
            const auto id = static_cast<std::uint64_t>(std::hash<std::string>{}(offer.id));
            card(ui, {x, row, transfer_width, 42});
            const auto note = offer.transferred ? readable_size(offer.transferred) + " / " + readable_size(offer.size) :
                offer.state == State::uploading ? "正在接收" : readable_size(offer.size);
            label(ui, offer.name, {x + 12, row + 2, transfer_width - 142, 22}, 13);
            label(ui, note, {x + 12, row + 22, transfer_width - 142, 16}, 11, muted);
            if (offer.state == State::pending) {
                if (button(ui, "接收", {x + transfer_width - 123, row + 7, 52, 28}, true, id)) {
                    if (!session.decide_offer(offer.id, true)) view.status = "请求已变化或超出会话接收上限";
                }
                if (button(ui, "拒绝", {x + transfer_width - 64, row + 7, 52, 28}, false, id + 1)) {
                    session.decide_offer(offer.id, false);
                }
            } else if (offer.state == State::approved) {
                label(ui, "等待上传", {x + transfer_width - 77, row + 7, 66, 28}, 12, online);
            }
        }
    }
    label(ui, "已分享文件", {x, row, transfer_width, 28}, 16, text, true);
    row += 34;
    if (snapshot.shared.empty()) {
        label(ui, "暂无分享文件", {x, row, transfer_width, 26}, 13, muted);
        return;
    }
    ui.scroll({x, row, transfer_width, std::max(45.0f, bottom - row)},
        static_cast<int>(snapshot.shared.size()), 44.0f, [&](int index, const Rect& box) {
            const auto& file = snapshot.shared[static_cast<std::size_t>(index)];
            card(ui, {box.x, box.y, box.w - 10, 38});
            label(ui, file.name + "  ·  " + readable_size(file.size), {box.x + 12, box.y + 2, box.w - 92, 34}, 13);
            if (button(ui, "移除", {box.x + box.w - 71, box.y + 5, 50, 28}, false,
                       std::hash<std::string>{}(file.id))) session.unshare(file.id);
        });
}

void render_nearby(Ui& ui, View& view, const std::vector<tempshare::Peer>& peers, float x, float width) {
    const auto first = page_controls(ui, view.peer_page, peers.size(), 7, x, 105, width);
    if (peers.empty()) label(ui, "正在查找同一局域网中运行 TempShare 的设备…", {x, 109, width, 32}, 14, muted);
    float y = 146;
    for (auto i = first; i < std::min(first + 7, peers.size()); ++i, y += 56) {
        const auto& peer = peers[i];
        card(ui, {x, y, width, 50});
        label(ui, peer.name, {x + 14, y + 3, width - 120, 24}, 15, text, true);
        label(ui, peer.platform + "  ·  " + peer.address, {x + 14, y + 26, width - 120, 17}, 12, muted);
        if (button(ui, "连接", {x + width - 85, y + 9, 70, 32}, false,
                   std::hash<std::string>{}(peer.url()))) {
            if (!open_url(peer.url())) view.status = "无法打开浏览器，请手动输入设备地址";
        }
    }
}

void render_received(Ui& ui, View& view, const tempshare::Snapshot& snapshot, float x, float width, float bottom) {
    const auto& files = snapshot.received;
    if (files.empty()) {
        label(ui, "本次运行还没有收到文件", {x, 109, width, 32}, 14, muted);
        return;
    }
    ui.scroll({x, 106, width, bottom - 106}, static_cast<int>(files.size()), 56.0f,
        [&](int index, const Rect& box) {
            const auto& file = files[files.size() - 1 - static_cast<std::size_t>(index)];
            card(ui, {box.x, box.y, box.w - 10, 50});
            label(ui, file.name + "  ·  " + readable_size(file.size), {box.x + 14, box.y + 3, box.w - 132, 24},
                  15, text, true);
            label(ui, file.path.u8string(), {box.x + 14, box.y + 26, box.w - 132, 17}, 12, muted);
            if (button(ui, "打开目录", {box.x + box.w - 109, box.y + 10, 84, 30}, false,
                       std::hash<std::string>{}(file.id)) && !open_folder(file.path.parent_path())) {
                view.status = "无法打开接收目录";
            }
        });
}

std::filesystem::path receive_directory(const Options& options, const std::filesystem::path& config) {
    if (!options.directory.empty()) return options.directory;
    if (!config.empty()) {
        std::ifstream input(config / "receive-directory", std::ios::binary);
        std::string value(16 * 1024, '\0');
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (input.eof() && input.gcount() > 0 && !input.bad()) {
            value.resize(static_cast<std::size_t>(input.gcount()));
            const auto path = std::filesystem::u8path(value);
            std::error_code error;
            if (path.is_absolute() && std::filesystem::is_directory(path, error)) return path;
        }
    }
    return std::filesystem::current_path() / "received";
}

bool save_receive_directory(const std::filesystem::path& config, const std::filesystem::path& directory) {
    if (config.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(config, error);
    if (error) return false;
    const auto temporary = config / ("receive-directory-" + tempshare::random_token() + ".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << directory.u8string();
    output.close();
    if (output) std::filesystem::rename(temporary, config / "receive-directory", error);
    if (output && !error) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

void render_settings(Ui& ui, View& view, tempshare::Session& session, const tempshare::Snapshot& snapshot,
                     const std::filesystem::path& config, float x, float width) {
    card(ui, {x, 106, width, 150});
    label(ui, "文件接收目录", {x + 18, 119, width - 36, 30}, 16, text, true);
    label(ui, snapshot.directory.u8string(), {x + 18, 151, width - 36, 28}, 13, muted);
    ui.input(&view.folder, {x + 18, 199, width - 220, 32}, Style().hint("输入目录路径"));
    const auto update_directory = [&](const std::filesystem::path& dir) {
        if (!session.set_directory(dir)) view.status = "请选择一个已存在的目录";
        else if (save_receive_directory(config, dir)) view.status = "已保存接收目录";
        else view.status = "本次接收目录已更新，无法写入本机配置";
    };
    if (button(ui, "保存", {x + width - 189, 199, 76, 32}) && !view.folder.empty()) {
        update_directory(std::filesystem::absolute(std::filesystem::u8path(view.folder)));
    }
    if (button(ui, "浏览", {x + width - 102, 199, 84, 32})) {
        const auto paths = pick_paths(view, true);
        if (!paths.empty()) update_directory(paths.front());
    }

    card(ui, {x, 274, width, 122});
    label(ui, "通过路径添加共享文件", {x + 18, 287, width - 36, 28}, 16, text, true);
    ui.input(&view.path, {x + 18, 340, width - 132, 32}, Style().hint("输入文件路径"));
    if (snapshot.active && button(ui, "添加", {x + width - 103, 340, 85, 32}, true) && !view.path.empty()) {
        share_paths(session, view, {std::filesystem::u8path(view.path)});
    }
    if (snapshot.active) {
        card(ui, {x, 414, width, 154});
        label(ui, "当前会话权限", {x + 18, 427, width - 36, 28}, 16, text, true);
        auto permissions = snapshot.permissions;
        ui.checkbox(&permissions.download, "允许对方下载共享文件", {x + 18, 465, width - 36, 25});
        ui.checkbox(&permissions.upload, "允许对方申请发送文件", {x + 18, 495, width - 36, 25});
        ui.checkbox(&permissions.text, "允许双向发送文本", {x + 18, 525, width - 36, 25});
        if (permissions.download != snapshot.permissions.download || permissions.upload != snapshot.permissions.upload ||
            permissions.text != snapshot.permissions.text) session.set_permissions(permissions);
    }
}

bool paired(const tempshare::Snapshot& snapshot) {
    return snapshot.pairing && snapshot.pairing->state == State::approved;
}

void render_devices(Ui& ui, tempshare::Session& session, const tempshare::Snapshot& snapshot,
                    View& view, float x, float width) {
    card(ui, {x, 106, width, 104});
    if (!snapshot.trust_enabled) {
        label(ui, "持久信任需要配置 HTTPS 和可写的本机配置目录。",
              {x + 18, 123, width - 36, 30}, 14, muted);
        return;
    }
    if (paired(snapshot)) {
        label(ui, "当前设备 · " + snapshot.pairing->name, {x + 18, 116, width - 36, 32}, 16, text, true);
        const bool known = !snapshot.pairing->trusted_key.empty();
        if (button(ui, known ? "更新信任权限" : "记住此设备", {x + 18, 158, 121, 34}, true)) {
            view.status = session.remember_device() ? "已保存设备和当前会话权限" : "无法保存设备信任";
        }
        label(ui, "保存设置页中的当前权限", {x + 154, 160, width - 172, 28}, 12, muted);
    } else {
        label(ui, "配对设备后，可在这里主动保存信任和授权范围。",
              {x + 18, 123, width - 36, 30}, 14, muted);
    }
    label(ui, "已记住的设备", {x, 223, width, 30}, 16, text, true);
    const auto first = page_controls(ui, view.device_page, snapshot.trusted.size(), 6, x, 253, width);
    float y = 291;
    for (auto i = first; i < std::min(first + 6, snapshot.trusted.size()); ++i, y += 54) {
        const auto& device = snapshot.trusted[i];
        card(ui, {x, y, width, 48});
        label(ui, device.name, {x + 14, y + 3, width - 126, 23}, 14, text, true);
        std::string permissions;
        if (device.permissions.download) permissions += "下载  ";
        if (device.permissions.upload) permissions += "申请上传  ";
        if (device.permissions.text) permissions += "文本";
        label(ui, permissions.empty() ? "仅建立连接" : permissions, {x + 14, y + 26, width - 126, 17}, 12, muted);
        if (button(ui, "撤销信任", {x + width - 100, y + 8, 85, 32}, false, std::hash<std::string>{}(device.key))) {
            view.status = session.revoke_device(device.key) ? "设备信任已撤销" : "无法删除设备信任";
        }
    }
}

void render_sync(Ui& ui, aino::Window& window, tempshare::Session& session,
                 const tempshare::Snapshot& snapshot, View& view, float x, float width) {
    card(ui, {x, 106, width, 193});
    label(ui, "发送文本、链接或代码", {x + 18, 118, width - 36, 30}, 16, text, true);
    if (!paired(snapshot) || !snapshot.permissions.text) {
        label(ui, paired(snapshot) ? "当前会话的文本权限已关闭" : "连接设备后即可发送",
              {x + 18, 151, width - 36, 30}, 14, muted);
        return;
    }
    ui.textarea(&view.draft, {x + 18, 155, width - 36, 86}, Style().hint("输入文本"));
    const auto send = [&session, &view](const std::string& value) {
        if (session.send_text(value)) {
            view.status = "文本已发送";
            view.message_page = 0;
            return true;
        }
        view.status = "请输入有效文本，上限为 32 KiB";
        return false;
    };
    if (button(ui, "发送", {x + 18, 252, 75, 32}, true) && send(view.draft)) view.draft.clear();
    if (button(ui, "发送剪贴板", {x + 103, 252, 105, 32})) {
        window.request_clipboard([&window, &session, invite = snapshot.invitation, send](std::string value) {
            if (session.invitation_active(invite)) send(value);
            window.wake();
        });
    }
    if (button(ui, "分享图片", {x + 218, 252, 97, 32})) share_paths(session, view, pick_paths(view, false));
    label(ui, "本会话的内容", {x, 313, width, 30}, 16, text, true);
    if (!snapshot.messages.empty() && button(ui, "清空", {x + width - 65, 313, 65, 30})) {
        session.clear_messages();
    }
    const auto first = page_controls(ui, view.message_page, snapshot.messages.size(), 3, x, 350, width);
    float y = 386;
    for (auto i = first; i < std::min(first + 3, snapshot.messages.size()); ++i, y += 80) {
        const auto& entry = snapshot.messages[snapshot.messages.size() - 1 - i];
        card(ui, {x, y, width, 72});
        label(ui, entry.from_desktop ? "本机" : "对方", {x + 13, y + 4, width - 32, 22}, 12, muted);
        ui.draw_text(entry.text, {x + 13, y + 28, width - 98, 37}, text, Style().font(13).top().wrap_text());
        if (button(ui, "复制", {x + width - 72, y + 22, 57, 28})) {
            view.status = window.try_set_clipboard(entry.text) ? "已复制" : "无法写入剪贴板";
        }
    }
}

void render_cast(Ui& ui, tempshare::Session& session, const tempshare::HttpServer& server,
                 const tempshare::Snapshot& snapshot, View& view, float x, float width) {
    card(ui, {x, 106, width, 190});
    label(ui, "共享屏幕或窗口", {x + 18, 120, width - 36, 32}, 17, text, true);
    if (!paired(snapshot)) {
        label(ui, "连接设备后即可共享", {x + 18, 156, width - 36, 28}, 14, muted);
        return;
    }
    label(ui, snapshot.viewer_answered ? "正在向对方投递" : snapshot.capture_ready ? "等待对方观看" :
          snapshot.casting ? "等待本机选择共享来源" : "当前未共享",
          {x + 18, 157, width - 36, 26}, 13, snapshot.capture_ready ? online : muted);
    if (!snapshot.capture_ready && button(ui, snapshot.casting ? "重新打开来源选择" : "选择共享来源",
               {x + 18, 210, 170, 36}, true)) {
        const auto key = session.open_cast();
        if (key.empty() || !open_url(server.url("127.0.0.1") + "cast#" + key)) {
            session.stop_cast();
            view.status = "无法打开本机浏览器";
        }
    }
    if (snapshot.casting && button(ui, "停止投递", {x + 202, 210, 100, 36})) {
        session.stop_cast();
        view.status = "投递已停止";
    }
}

int run(const Options& options) {
    aino::WindowConfig config;
    config.width = 1010;
    config.height = 705;
    config.title = "TempShare";
    config.min_width = 815;
    config.min_height = 680;
    config.app_id = "TempShare";

    std::vector<std::filesystem::path> dropped;
    aino::Window window(config);
    if (!window.ok()) throw std::runtime_error("无法创建 TempShare 窗口");
    auto theme = aino::Theme::light();
    theme.tint = accent;
    window.set_theme(theme);
#ifdef _WIN32
    enable_file_drop(dropped);
#endif
    const auto repaint = [&window] { window.wake(); };
    const auto config_path = configuration_directory();
    const auto trust_path = options.cert.empty() || config_path.empty() ? std::filesystem::path{} : config_path / "trusted";
    const auto inbox = receive_directory(options, config_path);
    tempshare::Session session(inbox, repaint, std::chrono::minutes(30), trust_path);
    tempshare::HttpServer http(session, options.cert, options.key);
    if (!http.start(options.port)) throw std::runtime_error("Unable to start TempShare HTTP service. Check TLS configuration.");
    tempshare::Discovery discovery(http.port(), !options.cert.empty(), [&http] { return http.networks(); }, repaint);
    session.start();

    // Refresh network addresses and wake Aino only when visible state changes.
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool exiting = false;
    std::thread timer([&] {
        tempshare::clean_interrupted_uploads(inbox);
        std::unique_lock lock(timer_mutex);
        while (!timer_cv.wait_for(lock, std::chrono::seconds(1), [&] { return exiting; })) {
            session.expire();
            const bool network_changed = http.update_networks(tempshare::lan_networks());
            if (network_changed) repaint();
        }
    });
    const auto stop_timer = [&] {
        {
            std::lock_guard lock(timer_mutex);
            exiting = true;
        }
        timer_cv.notify_one();
        timer.join();
    };

    View view;
    try {
        window.run([&](Ui& ui) {
            if (view.archive.valid() && view.archive.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                view.archive_worker.join();
                view.status = view.archive.get();
            }
            if (!dropped.empty()) share_paths(session, view, std::exchange(dropped, {}));
            auto snapshot = session.snapshot(view.page);
            const float height = static_cast<float>(window.height());
            const float width = static_cast<float>(window.width());
            constexpr float side_width = 170;
            constexpr float navigation_inset = 12;
            constexpr float navigation_width = side_width - navigation_inset * 2;
            constexpr float x = side_width + 28;
            const float rail_left = width - 284;
            const float content_width = rail_left - x - 18;
            ui.clear(background);
            ui.fill_rect({0, 0, side_width, height}, sidebar);
            ui.fill_rect({17, 22, 30, 30}, accent, 7);
            ui.draw_text("ϟ", {17, 22, 30, 30}, surface, Style().font(21).bold().center());
            label(ui, "TempShare", {56, 20, 104, 34}, 16, text, true);
            if (view.previous_invitation != snapshot.invitation) {
                view.previous_invitation = snapshot.invitation;
                view.draft.clear();
                view.message_page = 0;
            }
            const std::array<const char*, 7> titles{
                "文件分享", "内容同步", "屏幕投递", "附近设备", "接收记录", "设备信任", "设置"
            };
            constexpr std::array<View::Page, 7> pages{View::Page::share, View::Page::sync, View::Page::cast,
                View::Page::nearby, View::Page::received, View::Page::devices, View::Page::settings};
            for (std::size_t i = 0; i < titles.size(); ++i) {
                const auto page = pages[i];
                const float y = 80 + 42 * static_cast<float>(i) + (i >= 3 ? 21 : 0);
                const bool selected = page == view.page;
                auto style = Style().font(13).corner(7).id(i + 100);
                if (selected) style.background(Color{220, 235, 255}).color(accent);
                else style.secondary();
                if (ui.button(titles[i], {navigation_inset, y, navigation_width, 34}, style)) {
                    view.page = page;
                    snapshot = session.snapshot(view.page);
                    view.status.clear();
                }
            }
            ui.fill_rect({23, 212, 124, 1}, border);
            label(ui, "本地连接", {21, height - 81, 128, 22}, 12, muted);
            label(ui, snapshot.active ? "会话进行中" : "尚未开始会话",
                  {21, height - 56, 132, 24}, 12, snapshot.active ? online : muted);
            const auto page = view.page;
            render_connection(ui, window, session, http, snapshot, http.networks(), view, rail_left);
            if (page != view.page) snapshot = session.snapshot(view.page);
            const auto index = static_cast<std::size_t>(std::find(pages.begin(), pages.end(), view.page) - pages.begin());
            label(ui, titles[index], {x, 28, content_width, 50}, 28, text, true);

            switch (view.page) {
            case View::Page::all: break;
            case View::Page::share:
                render_share(ui, window, session, snapshot, view, x, content_width, height - 53);
                break;
            case View::Page::sync: render_sync(ui, window, session, snapshot, view, x, content_width); break;
            case View::Page::cast: render_cast(ui, session, http, snapshot, view, x, content_width); break;
            case View::Page::nearby: render_nearby(ui, view, discovery.peers(), x, content_width); break;
            case View::Page::received: render_received(ui, view, snapshot, x, content_width, height - 53); break;
            case View::Page::devices: render_devices(ui, session, snapshot, view, x, content_width); break;
            case View::Page::settings: render_settings(ui, view, session, snapshot, config_path, x, content_width); break;
            }
            label(ui, view.status, {x, height - 43, content_width, 27}, 12, muted);
        });
    } catch (...) {
        session.end();
        if (view.archive_worker.joinable()) view.archive_worker.join();
        stop_timer();
        throw;
    }
    stop_timer();
    session.end();
    if (view.archive_worker.joinable()) view.archive_worker.join();
    return 0;
}

}  // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        int argc = 0;
        auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("无法读取启动参数");
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(argv, LocalFree);
        std::vector<std::string> values;
        std::vector<char*> pointers;
        values.reserve(argc);
        for (int i = 0; i < argc; ++i) values.push_back(std::filesystem::path(argv[i]).u8string());
        for (auto& value : values) pointers.push_back(value.data());
        return run(parse_options(argc, pointers.data()));
    } catch (const std::exception& error) {
        const std::string description(error.what());
        const auto size = MultiByteToWideChar(CP_UTF8, 0, description.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        if (size > 0) MultiByteToWideChar(CP_UTF8, 0, description.c_str(), -1, wide.data(), size);
        MessageBoxW(nullptr, size > 0 ? wide.c_str() : L"TempShare 启动失败", L"TempShare", MB_OK | MB_ICONERROR);
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "TempShare: " << error.what() << '\n';
        return 1;
    }
}
#endif
