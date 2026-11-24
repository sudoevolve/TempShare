#include "core/discovery.hpp"
#include "core/folder_archive.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"
#include "platform.hpp"

#define AINO_IMPLEMENTATION
#include "aino.hpp"

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

const Color background{13, 17, 33};
const Color sidebar{20, 25, 44};
const Color surface{25, 32, 56};
const Color border{47, 58, 88};
const Color text{242, 245, 255};
const Color muted{169, 181, 212};
const Color accent{101, 112, 255};
const Color online{109, 228, 178};

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
    ui.fill_rect(box, surface, 14);
    ui.stroke_rect(box, border, 14);
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
    enum class Page { share, sync, cast, nearby, received, devices, settings };
    Page page = Page::share;
    std::string selected_address;
    std::size_t file_page = 0;
    std::size_t peer_page = 0;
    std::size_t received_page = 0;
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
    const auto paths = choose_paths(true);
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

void render_share(Ui& ui, aino::Window& window, tempshare::Session& session, const tempshare::HttpServer& server,
                  const tempshare::Snapshot& snapshot, const std::vector<tempshare::LocalNetwork>& networks,
                  View& view, float x, float width) {
    constexpr float connect_width = 256;
    const float transfer_width = width - connect_width - 18;
    const float qr_left = x + transfer_width + 18;
    card(ui, {qr_left, 106, connect_width, 430});
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
        if (snapshot.pairing && snapshot.pairing->state == State::approved) {
            label(ui, "浏览器已连接", {qr_left + 18, 177, 220, 32}, 17, online, true);
            label(ui, "结束会话后可连接另一台设备", {qr_left + 18, 219, 220, 24}, 12, muted);
        } else {
            if (networks.empty()) {
                label(ui, "等待局域网连接", {qr_left + 18, 165, 220, 32}, 16, text, true);
                label(ui, "连接网络后会自动更新二维码", {qr_left + 18, 201, 220, 25}, 12, muted);
            } else {
                draw_qr(ui, *view.qr, {qr_left + 25, 151, 206, 206});
            }
            label(ui, url, {qr_left + 18, 361, 220, 24}, 12, muted);
            label(ui, "手动连接码  " + snapshot.code, {qr_left + 18, 387, 220, 30}, 16, text, true);
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
        label(ui, "开始会话后可用手机扫码", {qr_left + 18, 170, 220, 30}, 13, muted);
        if (button(ui, "新建会话", {qr_left + 18, 217, 220, 38}, true)) {
            session.start();
            view.status.clear();
        }
    }

    card(ui, {x, 106, transfer_width, 120});
    label(ui, "发送文件", {x + 18, 119, transfer_width - 36, 30}, 16, text, true);
#ifdef _WIN32
    label(ui, "拖入文件，或选择要分享的文件", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#else
    label(ui, "选择文件，或在设置中输入文件路径", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#endif
    if (snapshot.active && button(ui, "选择文件", {x + 18, 181, 114, 32}, true)) {
        share_paths(session, view, choose_paths(false));
    }
    if (snapshot.active && !view.archive.valid() && button(ui, "分享文件夹", {x + 141, 181, 115, 32})) {
        share_folder(session, window, view, snapshot);
    }
    const auto pairing = snapshot.pairing;
    if (pairing && pairing->state == State::pending) {
        card(ui, {x, 239, transfer_width, 96});
        label(ui, pairing->name + " 申请连接", {x + 18, 246, transfer_width - 36, 29}, 15, text, true);
        label(ui, "核对浏览器验证码  " + pairing->verification, {x + 18, 275, transfer_width - 36, 23},
              13, online);
        if (button(ui, "允许", {x + 18, 302, 66, 26}, true)) session.decide_pairing(pairing->token, true);
        if (button(ui, "拒绝", {x + 93, 302, 66, 26})) session.decide_pairing(pairing->token, false);
    }
    const float list_y = pairing && pairing->state == State::pending ? 348.0f : 241.0f;
    label(ui, "待接收文件", {x, list_y, transfer_width, 26}, 16, text, true);
    std::vector<const tempshare::Offer*> pending;
    for (const auto& offer : snapshot.offers) {
        if (offer.state == State::pending || offer.state == State::approved || offer.state == State::uploading) {
            pending.push_back(&offer);
        }
    }
    const auto offer_start = page_controls(ui, view.offer_page, pending.size(), 2, x, list_y, transfer_width);
    float row = list_y + 32;
    if (pending.empty()) {
        label(ui, "暂无接收请求", {x, row, transfer_width, 26}, 13, muted);
        row += 36;
    } else {
        for (std::size_t i = offer_start; i < std::min(offer_start + 2, pending.size()); ++i, row += 48) {
            const auto& offer = *pending[i];
            const auto id = static_cast<std::uint64_t>(std::hash<std::string>{}(offer.id));
            card(ui, {x, row, transfer_width, 42});
            const auto note = offer.transferred ? readable_size(offer.transferred) + " / " + readable_size(offer.size) :
                offer.state == State::uploading ? "正在接收" : readable_size(offer.size);
            label(ui, offer.name, {x + 12, row + 2, transfer_width - 142, 22}, 13);
            label(ui, note, {x + 12, row + 22, transfer_width - 142, 16}, 11, muted);
            if (offer.state == State::pending) {
                if (button(ui, "接收", {x + transfer_width - 123, row + 7, 52, 28}, true, id)) {
                    session.decide_offer(offer.id, true);
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
    const auto first = page_controls(ui, view.file_page, snapshot.shared.size(), 3, x, row, transfer_width);
    row += 34;
    if (snapshot.shared.empty()) label(ui, "暂无分享文件", {x, row, transfer_width, 26}, 13, muted);
    for (std::size_t i = first; i < std::min(first + 3, snapshot.shared.size()); ++i, row += 44) {
        const auto& file = snapshot.shared[i];
        card(ui, {x, row, transfer_width, 38});
        label(ui, file.name + "  ·  " + readable_size(file.size),
              {x + 12, row + 2, transfer_width - 82, 34}, 13);
        if (button(ui, "移除", {x + transfer_width - 61, row + 5, 50, 28}, false,
                   std::hash<std::string>{}(file.id))) session.unshare(file.id);
    }
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

void render_received(Ui& ui, View& view, const tempshare::Snapshot& snapshot, float x, float width) {
    const auto& files = snapshot.received;
    const auto first = page_controls(ui, view.received_page, files.size(), 7, x, 105, width);
    if (files.empty()) label(ui, "本次运行还没有收到文件", {x, 109, width, 32}, 14, muted);
    float y = 146;
    for (auto i = first; i < std::min(first + 7, files.size()); ++i, y += 56) {
        const auto& file = files[files.size() - 1 - i];
        card(ui, {x, y, width, 50});
        label(ui, file.name + "  ·  " + readable_size(file.size), {x + 14, y + 3, width - 28, 24}, 15, text, true);
        label(ui, file.path.u8string(), {x + 14, y + 26, width - 28, 17}, 12, muted);
    }
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
        const auto paths = choose_paths(true);
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
        label(ui, paired(snapshot) ? "当前会话的文本权限已关闭" : "请先在文件分享页配对浏览器",
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
    if (button(ui, "分享图片", {x + 218, 252, 97, 32})) share_paths(session, view, choose_paths(false));
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
        label(ui, "请先在文件分享页配对观看设备", {x + 18, 156, width - 36, 28}, 14, muted);
        return;
    }
    label(ui, "在本机浏览器选择共享来源，再让对方打开网页的屏幕投递页。",
          {x + 18, 157, width - 36, 26}, 13, muted);
    if (button(ui, snapshot.casting ? "重新打开投递控制台" : "打开投递控制台",
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
    if (!window.ok()) return 1;
    auto theme = aino::Theme::dark();
    theme.tint = accent;
    window.set_theme(theme);
#ifdef _WIN32
    enable_file_drop(dropped);
#endif
    const auto repaint = [&window] { window.wake(); };
    const auto config_path = configuration_directory();
    const auto trust_path = options.cert.empty() || config_path.empty() ? std::filesystem::path{} : config_path / "trusted";
    tempshare::Session session(receive_directory(options, config_path), repaint, std::chrono::minutes(30), trust_path);
    tempshare::HttpServer http(session, options.cert, options.key);
    if (!http.start(options.port)) throw std::runtime_error("Unable to start TempShare HTTP service. Check TLS configuration.");
    tempshare::Discovery discovery(http.port(), !options.cert.empty(), [&http] { return http.networks(); }, repaint);
    session.start();

    // Refresh network addresses and wake Aino only when visible state changes.
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool exiting = false;
    std::thread timer([&] {
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
            const auto snapshot = session.snapshot();
            const float height = static_cast<float>(window.height());
            const float width = static_cast<float>(window.width());
            constexpr float side_width = 170;
            constexpr float x = side_width + 28;
            const float content_width = width - x - 28;
            ui.clear(background);
            ui.fill_rect({0, 0, side_width, height}, sidebar);
            ui.fill_rect_grad({17, 22, 30, 30}, Color{67, 185, 249}, accent, 9);
            ui.draw_text("ϟ", {17, 22, 30, 30}, text, Style().font(21).bold().center());
            label(ui, "TempShare", {56, 20, 104, 34}, 17, text, true);
            if (view.previous_invitation != snapshot.invitation) {
                view.previous_invitation = snapshot.invitation;
                view.draft.clear();
                view.message_page = 0;
            }
            const std::array<const char*, 7> titles{
                "文件分享", "内容同步", "屏幕投递", "附近设备", "接收记录", "设备信任", "设置"
            };
            for (std::size_t i = 0; i < titles.size(); ++i) {
                const auto page = static_cast<View::Page>(i);
                if (button(ui, titles[i], {17, 91 + 50 * static_cast<float>(i), 136, 40}, page == view.page)) {
                    view.page = page;
                    view.status.clear();
                }
            }
            label(ui, !options.cert.empty() ? "HTTPS" : "局域网 HTTP",
                  {20, height - 75, 130, 25}, 13, !options.cert.empty() ? online : muted);
            label(ui, snapshot.active ? "会话有效期 30 分钟" : "会话未开启", {20, height - 47, 130, 24}, 12, muted);
            label(ui, titles[static_cast<std::size_t>(view.page)], {x, 20, content_width, 44}, 28, text, true);
            const std::array<const char*, 7> subtitles{
                "在同一局域网内，把文件传到另一台设备",
                "发送文本、链接、代码和图片",
                "让已配对的设备观看你的屏幕",
                "选择设备，在浏览器中输入对方屏幕上的连接码",
                "已保存到本机的文件", "管理可快速重连的设备", "管理接收目录和会话权限"
            };
            label(ui, subtitles[static_cast<std::size_t>(view.page)], {x, 66, content_width, 26}, 14, muted);

            switch (view.page) {
            case View::Page::share:
                render_share(ui, window, session, http, snapshot, http.networks(), view, x, content_width);
                break;
            case View::Page::sync: render_sync(ui, window, session, snapshot, view, x, content_width); break;
            case View::Page::cast: render_cast(ui, session, http, snapshot, view, x, content_width); break;
            case View::Page::nearby: render_nearby(ui, view, discovery.peers(), x, content_width); break;
            case View::Page::received: render_received(ui, view, snapshot, x, content_width); break;
            case View::Page::devices: render_devices(ui, session, snapshot, view, x, content_width); break;
            case View::Page::settings: render_settings(ui, view, session, snapshot, config_path, x, content_width); break;
            }
            label(ui, view.status, {x, height - 43, content_width, 27}, 13, online);
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

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "TempShare: " << error.what() << '\n';
        return 1;
    }
}
