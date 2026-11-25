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
#include <ctime>
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
using aino::SystemIcon;
using aino::Ui;
using tempshare::State;

enum Appearance { system_appearance, light_appearance, dark_appearance };

struct Palette {
    Color background, sidebar, surface, border, text, muted, accent, online, selected;
};

const Palette dark_palette{{15, 19, 36}, {23, 28, 48}, {26, 32, 54}, {48, 57, 87},
    {242, 244, 255}, {169, 183, 215}, {139, 151, 255}, {85, 223, 160}, {75, 73, 226}};
const Palette light_palette{{243, 246, 253}, {232, 237, 249}, {255, 255, 255}, {211, 220, 240},
    {24, 32, 55}, {77, 92, 125}, {67, 63, 201}, {19, 116, 79}, {72, 69, 215}};
Palette palette = dark_palette;
const auto& [background, sidebar, surface, border, text, muted, accent, online, selected_background] = palette;
const Color white{255, 255, 255};

int load_appearance(const std::filesystem::path& config) {
    if (config.empty()) return system_appearance;
    std::ifstream input(config / "appearance");
    int value = system_appearance;
    return (input >> value) && value >= system_appearance && value <= dark_appearance ? value : system_appearance;
}

bool save_appearance(const std::filesystem::path& config, int appearance) {
    if (config.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(config, error);
    if (error) return false;
    std::ofstream output(config / "appearance");
    output << appearance;
    output.close();
    return static_cast<bool>(output);
}

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
    ui.fill_rect(box, surface, 13);
    ui.stroke_rect(box, border, 13);
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
    Page page = Page::home;
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
    std::future<std::string> folder_job;
    std::thread folder_worker;
    std::shared_ptr<std::atomic<bool>> folder_cancel;
    std::shared_ptr<std::atomic<std::size_t>> folder_scanned;
    std::vector<std::filesystem::path> dropped_folders;
    std::string last_qr_url;
    std::optional<qrcodegen::QrCode> qr;
    bool confirm_end = false;
    std::string nearby_address;
    bool connection_open = false;
    bool diagnostics_open = false;
    std::string diagnosis;
    std::future<std::string> diagnostic_job;
    std::filesystem::path space_directory;
    std::string space_status;
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

void share_paths(tempshare::Session& session, View& view, const std::vector<std::filesystem::path>& paths, bool image = false) {
    if (paths.empty()) return;
    std::size_t added = 0;
    for (const auto& path : paths) {
        if (session.share(std::filesystem::absolute(path), image)) ++added;
    }
    view.status = "已添加 " + std::to_string(added) + " 个文件";
    if (added < paths.size()) view.status += "，部分文件无法读取、超过 1 GiB 或列表已满";
    if (!image) view.page = View::Page::share;
}

void share_folder(tempshare::Session& session, aino::Window& window, View& view,
                  const tempshare::Snapshot& snapshot, bool zip, std::filesystem::path folder = {},
                  std::string refresh = {}) {
    if (view.folder_job.valid()) return;
    if (folder.empty()) {
        const auto paths = pick_paths(view, true);
        if (paths.empty()) return;
        folder = paths.front();
    }
    view.folder_cancel = std::make_shared<std::atomic<bool>>(false);
    view.folder_scanned = std::make_shared<std::atomic<std::size_t>>(0);
    std::packaged_task<std::string()> work([&session, folder = std::move(folder), invitation = snapshot.invitation,
                                          zip, refresh = std::move(refresh), cancel = view.folder_cancel,
                                          scanned = view.folder_scanned] {
        tempshare::FolderProgress progress;
        try {
            if (zip) {
                auto result = tempshare::archive_folder(folder, [&] { return *cancel || !session.invitation_active(invitation); });
                if (!*cancel && session.share_archive(invitation, std::move(result.name), std::move(result.directory))) {
                    return std::string("ZIP 已加入共享");
                }
            } else if (session.share_folder(invitation, folder, refresh, [&] { return cancel->load(); },
                [&](tempshare::FolderProgress value) { progress = value; *scanned = value.scanned; })) {
                auto status = std::string(refresh.empty() ? "已共享 " : "已更新 ") +
                    std::to_string(progress.scanned - progress.skipped) + " 个条目";
                if (!refresh.empty()) status += " · 新增 " + std::to_string(progress.added) + "，移除 " + std::to_string(progress.removed);
                if (progress.skipped) status += " · 跳过 " + std::to_string(progress.skipped) + " 个不可共享条目";
                return status;
            }
            if (*cancel) return std::string("文件夹任务已取消");
            return std::string("无法添加文件夹，请检查目录、会话和共享列表");
        } catch (const std::exception& error) {
            if (*cancel) return std::string("文件夹任务已取消");
            return std::string(zip ? "文件夹打包失败 · " : "无法读取文件夹 · ") + error.what();
        }
    });
    auto result = work.get_future();
    view.folder_worker = std::thread([&window, task = std::move(work)]() mutable {
        task();
        window.wake();
    });
    view.folder_job = std::move(result);
    view.status = zip ? "正在打包文件夹…" : "正在读取文件夹目录…";
}

void share_selection(tempshare::Session& session, View& view, const std::vector<std::filesystem::path>& paths) {
    std::vector<std::filesystem::path> files;
    for (const auto& path : paths) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) view.dropped_folders.push_back(path);
        else files.push_back(path);
    }
    share_paths(session, view, files);
    if (!view.dropped_folders.empty()) view.page = View::Page::share;
}

void render_connection(Ui& ui, aino::Window& window, tempshare::Session& session, const tempshare::HttpServer& server,
                       const tempshare::Snapshot& snapshot, const std::vector<tempshare::LocalNetwork>& networks,
                       View& view, float qr_left, float top) {
    card(ui, {qr_left, top, 256, 342});
    label(ui, snapshot.pairing ? "连接设备" : "扫码连接", {qr_left + 18, top + 12, 220, 26}, 16, text, true);
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
            label(ui, pairing.name, {qr_left + 18, top + 61, 220, 32}, 17, text, true);
            if (pairing.state == State::pending) {
                label(ui, "核对浏览器验证码 · " + std::to_string(pairing.remaining_seconds) + " 秒",
                      {qr_left + 18, top + 99, 220, 24}, 12, muted);
                label(ui, pairing.verification, {qr_left + 18, top + 130, 220, 42}, 27, online, true);
                if (button(ui, "允许连接", {qr_left + 18, top + 184, 104, 34}, true)) {
                    session.decide_pairing(pairing.token, true);
                }
                if (button(ui, "拒绝", {qr_left + 134, top + 184, 104, 34})) session.decide_pairing(pairing.token, false);
            } else {
                label(ui, "已连接", {qr_left + 18, top + 97, 220, 26}, 13, online, true);
                const auto waiting = std::count_if(snapshot.offers.begin(), snapshot.offers.end(),
                    [](const tempshare::Offer& offer) { return offer.state == State::pending; });
                if (waiting && button(ui, std::to_string(waiting) + " 个文件待接收",
                                      {qr_left + 18, top + 143, 220, 36}, true)) {
                    view.page = View::Page::share;
                    view.offer_page = 0;
                    window.wake();
                }
                if (snapshot.capture_ready) {
                    label(ui, "正在共享屏幕", {qr_left + 18, top + 187, 220, 27}, 13, online);
                    if (button(ui, "停止共享", {qr_left + 18, top + 219, 220, 33})) session.stop_cast();
                }
                if (button(ui, "会话权限", {qr_left + 18, top + 262, 103, 28})) view.page = View::Page::settings;
                if (button(ui, snapshot.trust_enabled ? "设备信任" : "信任设置", {qr_left + 134, top + 262, 104, 28})) {
                    view.page = View::Page::devices;
                }
            }
        } else {
            if (networks.empty()) {
                label(ui, "没有可用的局域网地址", {qr_left + 18, top + 60, 220, 32}, 15, text, true);
                label(ui, "请连接与手机相同的网络", {qr_left + 18, top + 97, 220, 25}, 12, muted);
            } else {
                draw_qr(ui, *view.qr, {qr_left + 40, top + 38, 176, 176});
            }
            if (!networks.empty()) {
                const auto link = url + " · " + (current->wireless ? "Wi-Fi" : "以太网");
                ui.draw_text(link, {qr_left + 12, top + 215, 232, 21}, muted, Style().center().font(11));
                ui.draw_text("连接码  " + snapshot.code, {qr_left + 18, top + 238, 220, 24},
                             text, Style().center().font(15).semi_bold());
            }
            if (!networks.empty() && button(ui, "复制链接", {qr_left + 18, top + 265, networks.size() > 1 ? 102.f : 220.f, 28})) {
                view.status = window.try_set_clipboard(invitation) ? "邀请链接已复制" : "无法写入剪贴板";
            }
            if (networks.size() > 1) {
                std::vector<std::string> choices;
                for (const auto& network : networks) {
                    choices.push_back(network.address + " · " + (network.interface_name.empty()
                        ? (network.wireless ? "Wi-Fi" : "以太网") : network.interface_name));
                }
                int index = static_cast<int>(std::distance(networks.begin(), current));
                if (ui.select(choices, &index, {qr_left + 132, top + 265, 106, 28}, Style().font(11))) {
                    view.selected_address = networks[static_cast<std::size_t>(index)].address;
                    window.wake();
                }
            }
        }
        const auto active_requests = snapshot.pending_transfers;
        if (view.confirm_end) {
            if (button(ui, "继续会话", {qr_left + 18, top + 301, 101, 28})) view.confirm_end = false;
            if (button(ui, "确认结束", {qr_left + 132, top + 301, 106, 28}, true)) {
                session.end();
                view.confirm_end = false;
                view.status = "会话已结束";
            }
        } else {
            if (button(ui, "延长 · " + std::to_string((snapshot.remaining_seconds + 59) / 60) + " 分",
                       {qr_left + 18, top + 301, 101, 28})) {
                if (session.extend()) view.status = "已从现在起延长会话 30 分钟";
            }
            if (button(ui, "结束会话", {qr_left + 132, top + 301, 106, 28})) {
                if (active_requests || snapshot.capture_ready) {
                    view.confirm_end = true;
                    view.status = "结束会话将停止投屏或取消 " + std::to_string(active_requests) + " 个接收请求";
                } else {
                    session.end();
                    view.status = "会话已结束";
                }
            }
        }
    } else {
        label(ui, "当前没有会话", {qr_left + 18, top + 64, 220, 30}, 13, muted);
        if (button(ui, "新建会话", {qr_left + 18, top + 111, 220, 38}, true)) {
            session.start();
            view.status.clear();
        }
    }
}

void render_share(Ui& ui, aino::Window& window, tempshare::Session& session,
                  const tempshare::Snapshot& snapshot, View& view, float x, float transfer_width, float bottom) {
    card(ui, {x, 106, transfer_width, 120});
    label(ui, "分享文件或文件夹", {x + 18, 119, transfer_width - 36, 30}, 16, text, true);
#ifdef _WIN32
    label(ui, "拖入文件，或选择要分享的文件", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#else
    label(ui, "选择文件，或在设置中输入文件路径", {x + 18, 151, transfer_width - 36, 25}, 13, muted);
#endif
    if (snapshot.active && button(ui, "选择文件", {x + 18, 181, 92, 32}, true)) {
        share_paths(session, view, pick_paths(view, false));
    }
    if (snapshot.active && !view.folder_job.valid()) {
        if (button(ui, "分享文件夹", {x + 119, 181, 101, 32})) share_folder(session, window, view, snapshot, false);
        if (button(ui, "打包 ZIP", {x + 229, 181, 86, 32})) share_folder(session, window, view, snapshot, true);
    } else if (view.folder_job.valid()) {
        if (button(ui, "取消目录任务", {x + 119, 181, 135, 32})) *view.folder_cancel = true;
        const auto scanned = view.folder_scanned->load();
        if (scanned && transfer_width > 355) {
            label(ui, "已读取 " + std::to_string(scanned) + " 项", {x + 262, 181, transfer_width - 275, 32}, 12, muted);
        }
    } else if (button(ui, "新建会话", {x + 119, 181, 101, 32})) {
        session.start();
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
    const auto* first = offer_start < pending.size() ? pending[offer_start] : nullptr;
    for (std::size_t i = 0; first && i < pending.size(); ++i) {
        const auto& candidate = *pending[i];
        const bool same_batch = first->batch.empty() ? i >= offer_start && i < offer_start + 3 :
            candidate.batch == first->batch;
        if (candidate.state == State::pending && same_batch) {
            visible.push_back(candidate.id);
            visible_bytes += candidate.size;
        }
    }
    if (visible.size() > 1 && button(ui, "接收 " + std::to_string(visible.size()) + " 个",
                                     {x + 104, list_y, 92, 28}, true)) {
        const auto count = session.decide_offers(visible, true);
        view.status = "已接受 " + std::to_string(count) + " 个文件";
        if (count < visible.size()) view.status += "，部分请求已变化或超出会话接收上限";
    }
    if (visible.size() > 1 && transfer_width >= 405 && button(ui, "拒绝这批", {x + 200, list_y, 73, 28})) {
        session.decide_offers(visible, false);
    }
    if (visible.size() > 1 && transfer_width >= 400) {
        label(ui, readable_size(visible_bytes), {x + 280, list_y, 68, 24}, 12, muted);
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
    label(ui, "已共享", {x, row, transfer_width, 28}, 16, text, true);
    row += 34;
    if (snapshot.shared.empty()) {
        label(ui, "暂无共享内容", {x, row, transfer_width, 26}, 13, muted);
        return;
    }
    ui.scroll({x, row, transfer_width, std::max(45.0f, bottom - row)},
        static_cast<int>(snapshot.shared.size()), 44.0f, [&](int index, const Rect& box) {
            const auto& file = snapshot.shared[static_cast<std::size_t>(index)];
            card(ui, {box.x, box.y, box.w - 10, 38});
            if (file.folder) ui.system_icon(SystemIcon::Folder, {box.x + 12, box.y + 9, 20, 20}, accent);
            const float name_inset = file.folder ? 40.0f : 12.0f;
            const float actions = file.folder ? 137.0f : 80.0f;
            label(ui, file.name + "  ·  " + (file.folder ? "可浏览的文件夹" : readable_size(file.size)),
                  {box.x + name_inset, box.y + 2, box.w - actions - name_inset, 34}, 13);
            if (file.folder && !view.folder_job.valid() && button(ui, "刷新", {box.x + box.w - 128, box.y + 5, 50, 28},
                       false, std::hash<std::string>{}("refresh" + file.id))) {
                share_folder(session, window, view, snapshot, false, file.path, file.id);
            }
            if (button(ui, "移除", {box.x + box.w - 71, box.y + 5, 50, 28}, false,
                       std::hash<std::string>{}(file.id))) session.unshare(file.id);
        });
}

void render_nearby(Ui& ui, View& view, tempshare::Discovery& discovery, bool nearby_available, float x, float width) {
    const auto peers = discovery.peers();
    ui.input(&view.nearby_address, {x, 106, width - 94, 31}, Style().hint("局域网地址，如 192.168.1.6:8787"));
    if (button(ui, "打开地址", {x + width - 85, 106, 85, 31})) {
        auto url = view.nearby_address;
        if (url.find("://") == std::string::npos) url = "http://" + url;
        const auto start = url.find("://") + 3;
        const auto end = url.find_first_of(":/", start);
        const bool http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
        if (!http || !tempshare::is_private_ipv4(url.substr(start, end - start)) ||
            url.find_first_of("@\\?# \r\n\t") != std::string::npos || !open_url(url)) {
            view.status = "请输入桌面端显示的局域网 IPv4 地址与端口";
        }
    }
    const auto first = page_controls(ui, view.peer_page, peers.size(), 6, x, 146, width);
    if (button(ui, "刷新", {x, 151, 62, 26})) {
        discovery.refresh();
        view.status = nearby_available ? "已重新发送局域网发现请求" : "没有可用的局域网地址";
    }
    if (peers.empty()) label(ui, nearby_available ? "目前没有发现其他 TempShare 设备" : "未连接可用的局域网",
                            {x + 73, 149, width - 75, 32}, 13, muted);
    float y = 183;
    for (auto i = first; i < std::min(first + 6, peers.size()); ++i, y += 56) {
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

void render_received(Ui& ui, aino::Window& window, tempshare::Session& session, View& view,
                     const tempshare::Snapshot& snapshot, float x, float width, float bottom) {
    const auto& files = snapshot.received;
    if (files.empty()) {
        label(ui, "暂无接收记录", {x, 109, width, 32}, 14, muted);
        return;
    }
    if (button(ui, "清理记录", {x + width - 85, 103, 85, 29})) {
        view.status = session.clear_received() ? "接收记录已清理，文件仍保存在接收目录" : "无法保存记录";
    }
    ui.scroll({x, 143, width, bottom - 143}, static_cast<int>(files.size()), 88.0f,
        [&](int index, const Rect& box) {
            const auto& file = files[files.size() - 1 - static_cast<std::size_t>(index)];
            card(ui, {box.x, box.y, box.w - 10, 82});
            label(ui, file.name + "  ·  " + readable_size(file.size), {box.x + 14, box.y + 3, box.w - 35, 24},
                  15, text, true);
            std::error_code error;
            const bool available = std::filesystem::is_regular_file(file.path, error);
            std::ostringstream summary;
            summary << file.received_from;
            if (file.received_at) {
                const auto time = static_cast<std::time_t>(file.received_at);
                std::tm local{};
#ifdef _WIN32
                if (localtime_s(&local, &time) == 0) summary << " · " << std::put_time(&local, "%m-%d %H:%M");
#else
                if (localtime_r(&time, &local)) summary << " · " << std::put_time(&local, "%m-%d %H:%M");
#endif
            }
            if (!available) summary << " · 文件已移动或删除";
            label(ui, summary.str(), {box.x + 14, box.y + 25, box.w - 35, 17}, 12, muted);
            const auto id = std::hash<std::string>{}(file.id);
            if (available && button(ui, "打开", {box.x + 14, box.y + 48, 56, 26}, false, id) &&
                !open_folder(file.path)) view.status = "无法打开文件";
            if (available && button(ui, "定位", {box.x + 78, box.y + 48, 56, 26}, false, id + 1) &&
                !reveal_file(file.path)) view.status = "无法定位文件";
            if (button(ui, "复制路径", {box.x + 142, box.y + 48, 77, 26}, false, id + 2)) {
                view.status = window.try_set_clipboard(file.path.u8string()) ? "路径已复制" : "无法写入剪贴板";
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
    return (config.empty() ? std::filesystem::current_path() : config) / "received";
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
    if (view.space_directory != snapshot.directory) {
        view.space_directory = snapshot.directory;
        std::error_code error;
        const auto disk = std::filesystem::space(snapshot.directory, error);
        view.space_status = error ? "接收目录将在首次保存时创建" : "可用空间 " + readable_size(disk.available);
    }
    label(ui, view.space_status, {x + 18, 177, width - 125, 20}, 11, muted);
    ui.input(&view.folder, {x + 18, 207, width - 220, 32}, Style().hint("输入目录路径"));
    const auto update_directory = [&](const std::filesystem::path& dir) {
        if (!session.set_directory(dir)) view.status = "请选择一个已存在且可写的目录";
        else if (save_receive_directory(config, dir)) view.status = "已保存接收目录";
        else view.status = "本次接收目录已更新，无法写入本机配置";
    };
    if (button(ui, "保存", {x + width - 189, 207, 76, 32}) && !view.folder.empty()) {
        update_directory(std::filesystem::absolute(std::filesystem::u8path(view.folder)));
    }
    if (button(ui, "浏览", {x + width - 102, 207, 84, 32})) {
        const auto paths = pick_paths(view, true);
        if (!paths.empty()) update_directory(paths.front());
    }
    if (button(ui, "恢复默认", {x + width - 103, 167, 85, 28})) {
        const auto target = (config.empty() ? std::filesystem::current_path() : config) / "received";
        std::error_code error;
        std::filesystem::create_directories(target, error);
        if (!error) update_directory(target);
        else view.status = "无法创建默认接收目录";
    }

    card(ui, {x, 274, width, 122});
    label(ui, "通过路径添加共享", {x + 18, 287, width - 36, 28}, 16, text, true);
    ui.input(&view.path, {x + 18, 340, width - 132, 32}, Style().hint("输入文件或文件夹路径"));
    if (snapshot.active && button(ui, "添加", {x + width - 103, 340, 85, 32}, true) && !view.path.empty()) {
        share_selection(session, view, {std::filesystem::u8path(view.path)});
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
    if (button(ui, "分享图片", {x + 218, 252, 97, 32})) share_paths(session, view, pick_paths(view, false), true);
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
        ui.draw_text(entry.attachment.empty() ? entry.text : "图片 · " + entry.text,
                     {x + 13, y + 28, width - 98, 37}, text, Style().font(13).top().wrap_text());
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
    label(ui, snapshot.cast_compatibility ? "兼容播放 · 画面通过局域网页面传输" :
          snapshot.viewer_answered ? "已完成媒体协商" : snapshot.capture_ready ? "等待对方观看" :
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

void render_home(Ui& ui, View& view, const std::vector<tempshare::Peer>& peers,
                 float x, float width, float nearby_width) {
    struct Feature {
        const char* title;
        const char* detail;
        const char* action;
        View::Page page;
        SystemIcon icon;
        Color top, bottom, tint;
    };
    const std::array<Feature, 3> features{{
        {"文件分享", "传输文件和文件夹", "开始分享", View::Page::share, SystemIcon::Folder,
         {45, 65, 138}, {29, 38, 91}, {95, 186, 255}},
        {"内容同步", "发送文本、链接或图片", "立即发送", View::Page::sync, SystemIcon::Text,
         {69, 55, 135}, {42, 35, 89}, {184, 155, 255}},
        {"屏幕投递", "共享屏幕或窗口", "开始投递", View::Page::cast, SystemIcon::Play,
         {39, 76, 131}, {25, 46, 75}, {114, 200, 255}},
    }};
    constexpr float gap = 14;
    const float tile_width = (width - gap * 2) / 3;
    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto& item = features[i];
        const float left = x + static_cast<float>(i) * (tile_width + gap);
        const Rect tile{left, 106, tile_width, 166};
        ui.fill_rect_grad(tile, item.top, item.bottom, 15);
        ui.stroke_rect_top(tile, Color{255, 255, 255, 42}, 15, 1, 16);
        ui.fill_circle(tile.right() - 34, 145, 23, Color{255, 255, 255, 26});
        ui.system_icon(item.icon, {tile.right() - 50, 129, 32, 32}, item.tint);
        label(ui, item.title, {left + 16, 157, tile_width - 28, 29}, 18, white, true);
        label(ui, item.detail, {left + 16, 187, tile_width - 28, 21}, 12, Color{220, 229, 251});
        if (ui.button(item.action, {left + 16, 224, 95, 28},
                      Style().id(200 + i).secondary().background(Color{255, 255, 255, 37})
                          .color(white).corner(14).font(12))) {
            view.page = item.page;
            view.status.clear();
        }
    }
    label(ui, "附近的设备", {x, 301, nearby_width - 100, 35}, 18, text, true);
    if (ui.button("查看全部", {x + nearby_width - 86, 304, 86, 28}, Style().ghost().color(accent).font(12))) {
        view.page = View::Page::nearby;
    }
    if (peers.empty()) {
        card(ui, {x, 348, nearby_width, 84});
        ui.system_icon(SystemIcon::Network, {x + 16, 371, 34, 34}, accent);
        label(ui, "正在查找附近的 TempShare 设备", {x + 64, 357, nearby_width - 80, 29}, 14, text, true);
        label(ui, "也可用手机扫描右侧二维码", {x + 64, 386, nearby_width - 80, 22}, 12, muted);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(peers.size(), 4); ++i) {
        const auto& peer = peers[i];
        const float y = 348 + static_cast<float>(i) * 62;
        card(ui, {x, y, nearby_width, 54});
        ui.system_icon(SystemIcon::Network, {x + 15, y + 13, 28, 28}, accent);
        label(ui, peer.name, {x + 55, y + 4, nearby_width - 146, 24}, 14, text, true);
        label(ui, peer.platform + " · " + peer.address, {x + 55, y + 28, nearby_width - 146, 19}, 12, muted);
        if (button(ui, "连接", {x + nearby_width - 75, y + 12, 61, 30}, false,
                   std::hash<std::string>{}(peer.url())) && !open_url(peer.url())) {
            view.status = "无法打开浏览器";
        }
    }
}

struct Connection {
    tempshare::Session session;
    tempshare::HttpServer http;
    View view;
#ifdef _WIN32
    tempshare::NotificationState last_notice;
#endif

    Connection(const Options& options, const std::filesystem::path& inbox, const std::filesystem::path& config,
               const std::function<void()>& repaint, std::size_t index)
        : session(inbox, repaint, std::chrono::minutes(30),
                  options.cert.empty() || config.empty() ? std::filesystem::path{} :
                      config / (index ? "trusted-" + std::to_string(index + 1) : "trusted"),
                  config.empty() ? std::filesystem::path{} :
                      config / (index ? "received-history-" + std::to_string(index + 1) : "received-history")),
          http(session, options.cert, options.key) {
        const unsigned preferred = options.port + static_cast<unsigned>(index);
        const auto port = static_cast<unsigned short>(preferred <= 65535 ? preferred : 0);
        if (!http.start(port)) throw std::runtime_error("无法启动局域网服务，请检查端口与证书。");
        session.start();
#ifdef _WIN32
        last_notice = session.notification_state();
#endif
    }

    ~Connection() {
        if (view.folder_worker.joinable()) {
            if (view.folder_cancel) *view.folder_cancel = true;
            view.folder_worker.join();
        }
    }
};

int run(const Options& options) {
    aino::WindowConfig config;
    config.width = 1100;
    config.height = 720;
    config.title = "TempShare";
    config.min_width = 860;
    config.min_height = 680;
    config.app_id = "TempShare";

    std::vector<std::filesystem::path> dropped;
    aino::Window window(config);
    if (!window.ok()) throw std::runtime_error("无法创建 TempShare 窗口");
#ifdef _WIN32
    DesktopIntegration desktop(dropped);
#endif
    const auto repaint = [&window] { window.wake(); };
    const auto config_path = configuration_directory();
    int appearance = load_appearance(config_path);
    bool system_dark = system_dark_mode().value_or(true);
    auto next_appearance_check = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<bool> applied_dark;
    const auto inbox = receive_directory(options, config_path);
#ifdef _WIN32
    desktop.inbox = inbox;
    const auto automatic_updates = config_path / "github-update.enabled";
    auto next_update = std::chrono::steady_clock::now();
#endif
    constexpr std::size_t connection_limit = 4;
    std::array<std::shared_ptr<Connection>, connection_limit> connections;
    connections[0] = std::make_shared<Connection>(options, inbox, config_path, repaint, 0);
    const auto primary = connections.front();
    tempshare::Discovery discovery(primary->http.port(), !options.cert.empty(),
                                   [primary] { return primary->http.networks(); }, repaint);
    int selected_connection = 0;

    // Refresh network addresses and periodically check for system appearance changes.
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool exiting = false;
    std::thread timer([&] {
        tempshare::clean_interrupted_uploads(inbox);
        std::unique_lock lock(timer_mutex);
        unsigned seconds = 0;
        while (!timer_cv.wait_for(lock, std::chrono::seconds(1), [&] { return exiting; })) {
            const auto snapshot = connections;
            lock.unlock();
            const auto networks = tempshare::lan_networks();
            bool network_changed = false;
            for (const auto& connection : snapshot) {
                if (!connection) continue;
                connection->session.expire();
                network_changed = connection->http.update_networks(networks) || network_changed;
            }
            ++seconds;
            if (network_changed || seconds % 3 == 0) repaint();
            lock.lock();
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

    try {
        window.run([&](Ui& ui) {
            const auto connection = connections[static_cast<std::size_t>(selected_connection)];
            auto& session = connection->session;
            auto& http = connection->http;
            auto& view = connection->view;
            if (view.folder_job.valid() && view.folder_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                view.folder_worker.join();
                view.status = view.folder_job.get();
            }
            if (!dropped.empty() && session.is_active()) share_selection(session, view, std::exchange(dropped, {}));
            if (!view.folder_job.valid() && !view.dropped_folders.empty()) {
                const auto current = session.snapshot(View::Page::home);
                if (current.active) {
                    auto folder = std::move(view.dropped_folders.front());
                    view.dropped_folders.erase(view.dropped_folders.begin());
                    share_folder(session, window, view, current, false, std::move(folder));
                } else {
                    view.dropped_folders.clear();
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (view.diagnostic_job.valid() && view.diagnostic_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                view.diagnosis = view.diagnostic_job.get();
            }
#ifdef _WIN32
            if (now >= next_update) {
                next_update = now + std::chrono::hours(1);
                std::error_code error;
                if (!config_path.empty() && std::filesystem::is_regular_file(automatic_updates, error)) desktop.updates(true);
            }
            desktop.inbox = session.notification_state().directory;
            for (const auto& item : connections) {
                if (!item) continue;
                const auto notification = item->session.notification_state();
                const auto& last = item->last_notice;
                if (notification.pairing != last.pairing && !notification.pairing.empty()) {
                    desktop.notify(L"TempShare · 连接请求", L"切换到待确认的连接，核对手机验证码。");
                } else if (notification.received != last.received && !notification.received.empty()) {
                    desktop.notify(L"TempShare · 文件已接收", L"文件已保存，点击打开 TempShare。");
                } else if (notification.offer != last.offer && !notification.offer.empty()) {
                    desktop.notify(L"TempShare · 文件接收请求", L"打开 TempShare 查看并确认待接收文件。");
                }
                item->last_notice = notification;
            }
#endif
            if (appearance == system_appearance && now >= next_appearance_check) {
                system_dark = system_dark_mode().value_or(true);
                next_appearance_check = now + std::chrono::seconds(3);
            }
            const bool dark = appearance == system_appearance ? system_dark : appearance == dark_appearance;
            if (applied_dark != dark) {
                palette = dark ? dark_palette : light_palette;
                auto theme = dark ? aino::Theme::dark() : aino::Theme::light();
                theme.window_bg = background;
                theme.set_tint(selected_background);
                window.set_theme(theme);
                applied_dark = dark;
            }
            auto snapshot = session.snapshot(view.page);
            const float height = static_cast<float>(window.height());
            const float width = static_cast<float>(window.width());
            constexpr float side_width = 170;
            constexpr float navigation_inset = 12;
            constexpr float navigation_width = side_width - navigation_inset * 2;
            constexpr float x = side_width + 28;
            const float rail_left = width - 284;
            const bool auto_hide = width < 990 && view.page != View::Page::home && !snapshot.pairing &&
                !view.connection_open && !view.diagnostics_open;
            const float content_width = auto_hide ? width - x - 28 : rail_left - x - 18;
            ui.clear(background);
            ui.fill_rect({0, 0, side_width, height}, sidebar);
            ui.fill_rect_grad({17, 22, 30, 30}, Color{59, 170, 255}, Color{104, 65, 236}, 9);
            ui.draw_text("ϟ", {17, 22, 30, 30}, white, Style().font(21).bold().center());
            label(ui, "TempShare", {56, 20, 104, 34}, 16, text, true);
            if (view.previous_invitation != snapshot.invitation) {
                view.previous_invitation = snapshot.invitation;
                view.draft.clear();
                view.message_page = 0;
                view.confirm_end = false;
            }
            const std::array<const char*, 8> titles{
                "首页", "文件分享", "内容同步", "屏幕投递", "附近设备", "接收记录", "设备信任", "设置"
            };
            constexpr std::array<View::Page, 8> pages{View::Page::home, View::Page::share, View::Page::sync, View::Page::cast,
                View::Page::nearby, View::Page::received, View::Page::devices, View::Page::settings};
            constexpr std::array<SystemIcon, 8> icons{SystemIcon::Card, SystemIcon::Folder, SystemIcon::Text,
                SystemIcon::Play, SystemIcon::Network, SystemIcon::Download, SystemIcon::People, SystemIcon::Settings};
            constexpr float nav_top = 81, nav_step = 40, nav_height = 34, group_gap = 16;
            constexpr std::size_t group_start = 4;
            for (std::size_t i = 0; i < titles.size(); ++i) {
                const auto page = pages[i];
                const float y = nav_top + nav_step * static_cast<float>(i) + (i >= group_start ? group_gap : 0);
                const bool selected = page == view.page;
                auto style = Style().ghost().font(13).corner(8).id(i + 100).left().padded(43, 0);
                if (selected) style.background(selected_background).color(white).semi_bold();
                else style.color(muted);
                if (ui.button(titles[i], {navigation_inset, y, navigation_width, nav_height}, style)) {
                    view.page = page;
                    snapshot = session.snapshot(view.page);
                    view.status.clear();
                }
                ui.system_icon(icons[i], {navigation_inset + 14, y + 9, 16, 16}, selected ? white : muted);
            }
            constexpr float divider = nav_top + nav_step * group_start + (group_gap + nav_height - nav_step) / 2;
            ui.fill_rect({23, divider, side_width - 46, 1}, border);
            std::vector<std::string> devices;
            for (const auto& item : connections) {
                if (!item) break;
                const auto state = item->session.snapshot(View::Page::home);
                auto name = "连接 " + std::to_string(devices.size() + 1) + " · ";
                name += state.pairing ? (state.pairing->state == State::pending ? "待确认" : state.pairing->name) :
                    state.active ? "待配对" : "已结束";
                devices.push_back(std::move(name));
            }
            if (ui.select(devices, &selected_connection, {navigation_inset, 447, navigation_width, 29}, Style().font(12))) {
                window.wake();
            }
            if (button(ui, "＋ 新建连接", {navigation_inset, 486, navigation_width, 29})) {
                std::size_t slot = 0;
                while (slot < connections.size() && connections[slot] && connections[slot]->session.is_active()) ++slot;
                if (slot == connections.size()) view.status = "最多同时使用 4 个连接，可先结束一个";
                else {
                    try {
                        if (connections[slot]) {
                            connections[slot]->session.start();
                            connections[slot]->view.page = View::Page::home;
                        } else {
                            auto next = std::make_shared<Connection>(options, snapshot.directory, config_path, repaint, slot);
                            std::lock_guard lock(timer_mutex);
                            connections[slot] = std::move(next);
                        }
                        selected_connection = static_cast<int>(slot);
                        window.wake();
                    } catch (const std::exception& error) { view.status = error.what(); }
                }
            }
            ui.fill_circle(23, height - 70, 3.5f, snapshot.active ? online : muted);
            label(ui, snapshot.active ? "局域网会话开启" : "尚未开始会话",
                  {34, height - 82, 125, 24}, 12, snapshot.active ? online : muted);
            static const std::vector<std::string> appearances{"系统", "浅色", "深色"};
            if (ui.segmented(appearances, &appearance, {navigation_inset, height - 50, navigation_width, 30},
                             Style().secondary().font(11))) {
                next_appearance_check = now;
                if (!save_appearance(config_path, appearance)) view.status = "本次外观已更新，无法保存偏好";
                window.wake();
            }
            const auto page = view.page;
            if (!auto_hide && !view.diagnostics_open) {
                render_connection(ui, window, session, http, snapshot, http.networks(), view, rail_left,
                                  page == View::Page::home ? 301.f : 106.f);
            }
            if (view.diagnostics_open) {
                card(ui, {rail_left, 106, 256, 342});
                label(ui, "连接诊断", {rail_left + 16, 117, 222, 28}, 16, text, true);
                ui.draw_text(view.diagnosis, {rail_left + 16, 152, 222, 234}, muted,
                             Style().font(12).left().top().wrap_text());
                if (button(ui, "复制", {rail_left + 16, 403, 101, 29}) && !view.diagnostic_job.valid()) {
                    view.status = window.try_set_clipboard(view.diagnosis) ? "连接诊断已复制" : "无法复制连接诊断";
                }
                if (button(ui, "返回连接", {rail_left + 132, 403, 106, 29})) view.diagnostics_open = false;
            } else if (auto_hide) {
                if (button(ui, "连接设备", {width - 120, 44, 92, 30})) view.connection_open = true;
            } else {
                const float diagnostic_y = page == View::Page::home ? 650.0f : 463.0f;
                if (button(ui, "连接诊断", {rail_left + 18, diagnostic_y, 103, 25})) {
                    view.diagnostics_open = true;
                    view.diagnosis = "正在测试本机监听和当前局域网地址…";
                    if (!view.diagnostic_job.valid()) view.diagnostic_job = std::async(std::launch::async, [&http, &window] {
                        const auto networks = http.networks();
                        std::string diagnosis = "TempShare " TEMPSHARE_VERSION "\n监听端口 " + std::to_string(http.port()) +
                            "\n本机服务 " + (http.probe("127.0.0.1") ? "可访问" : "无响应");
                        if (networks.empty()) diagnosis += "\n未发现可用私有 IPv4";
                        for (std::size_t i = 0; i < std::min<std::size_t>(networks.size(), 4); ++i) {
                            const auto& network = networks[i];
                            diagnosis += "\n" + http.url(network.address) + (http.probe(network.address) ? " 可访问" : " 无响应");
                        }
                        diagnosis += "\n手机无法打开时，请核对防火墙入站许可和访客网络隔离。";
                        window.wake();
                        return diagnosis;
                    });
                }
                if (width < 990 && page != View::Page::home && !snapshot.pairing &&
                    button(ui, "收起连接栏", {rail_left + 132, diagnostic_y, 106, 25})) view.connection_open = false;
            }
            if (page != view.page) snapshot = session.snapshot(view.page);
            const auto index = static_cast<std::size_t>(std::find(pages.begin(), pages.end(), view.page) - pages.begin());
            label(ui, view.page == View::Page::home ? "你好，TempShare" : titles[index],
                  {x, 24, content_width, 50}, 29, text, true);
            if (view.page == View::Page::home) {
                label(ui, "选择一种方式开始分享", {x, 69, content_width, 22}, 13, muted);
            }

            switch (view.page) {
            case View::Page::all: break;
            case View::Page::home:
                render_home(ui, view, discovery.peers(), x, width - x - 28, content_width);
                if (page != view.page) window.wake();
                break;
            case View::Page::share:
                render_share(ui, window, session, snapshot, view, x, content_width, height - 53);
                break;
            case View::Page::sync: render_sync(ui, window, session, snapshot, view, x, content_width); break;
            case View::Page::cast: render_cast(ui, session, http, snapshot, view, x, content_width); break;
            case View::Page::nearby: render_nearby(ui, view, discovery, !http.networks().empty(), x, content_width); break;
            case View::Page::received: render_received(ui, window, session, view, snapshot, x, content_width, height - 53); break;
            case View::Page::devices: render_devices(ui, session, snapshot, view, x, content_width); break;
            case View::Page::settings:
                render_settings(ui, view, session, snapshot, config_path, x, content_width);
#ifdef _WIN32
                label(ui, "TempShare " TEMPSHARE_VERSION, {x, 585, 123, 32}, 13, muted);
                if (button(ui, "检查更新", {x + 128, 585, 86, 32}) && !desktop.updates()) {
                    view.status = "无法启动更新检查";
                }
                if (button(ui, "缩到托盘", {x + 224, 585, 86, 32})) desktop.minimize();
#endif
                break;
            }
            label(ui, view.status, {x, height - 43, content_width, 27}, 12, muted);
        });
    } catch (...) {
        stop_timer();
        for (const auto& connection : connections) if (connection) connection->session.end();
        throw;
    }
    stop_timer();
    for (const auto& connection : connections) if (connection) connection->session.end();
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
