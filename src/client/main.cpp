#include "core/discovery.hpp"
#include "core/folder_archive.hpp"
#include "core/http_server.hpp"
#include "core/session.hpp"
#include "core/peer_link.hpp"
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

constexpr std::array<int, 5> session_durations{0, 30, 120, 480, 1440};
const std::vector<std::string> session_duration_names{"不限时 · 手动结束", "30 分钟", "2 小时", "8 小时", "24 小时"};

int load_session_minutes(const std::filesystem::path& config) {
    std::ifstream input(config / "session-duration");
    int minutes = 0;
    if (!(input >> minutes) || std::find(session_durations.begin(), session_durations.end(), minutes) == session_durations.end()) return 0;
    return minutes;
}

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

bool button(Ui& ui, const std::string& value, Rect rect, bool primary = false, std::uint64_t id = 0, bool enabled = true) {
    auto style = Style().font(13).corner(8).id(id);
    if (primary) style.primary();
    else style.secondary();
    if (!enabled) style.off();
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

std::string readable_time(std::uint64_t timestamp) {
    if (!timestamp || timestamp > 32535215999ULL) return "时间未知";
    const auto value = static_cast<std::time_t>(timestamp);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0) return "时间未知";
#else
    if (!localtime_r(&value, &local)) return "时间未知";
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%m-%d %H:%M");
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
    std::string status;
    std::string path;
    std::string folder;
    std::string draft;
    std::string previous_invitation;
    std::uint64_t last_read_messages = 0;
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
    std::string message_search;
    std::optional<tempshare::Message> message_detail;
    bool confirm_clear = false;
    bool confirm_leave = false;
    bool remote_files = false;
    std::string detail_text;
    std::string device_name;
    std::vector<std::filesystem::path> failed_files;
    std::vector<std::string> batch_ids;
    bool batch_open = false;
    bool batch_accept = true;
    bool downloads_open = false;
    std::optional<tempshare::Permissions> permissions_pending;
    std::string file_failures;
};

std::vector<std::filesystem::path> pick_paths(View& view, bool directory) {
    bool failed = false;
    auto paths = choose_paths(directory, &failed);
    if (failed) view.status = "无法打开文件选择器，可在文件页输入共享路径，或在设置中输入接收目录";
    return paths;
}

void share_paths(tempshare::Session& session, View& view, const std::vector<std::filesystem::path>& paths, bool image = false) {
    if (paths.empty()) return;
    std::size_t added = 0;
    view.failed_files.clear(); view.file_failures.clear();
    for (const auto& path : paths) {
        if (session.share(std::filesystem::absolute(path), image)) ++added;
        else {
            view.failed_files.push_back(path);
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            view.file_failures += path.u8string() + " · " + (error ? "无法读取或不是文件" : size > tempshare::max_file_size ? "超过1GiB" : "会话已结束或共享清单已满") + "\n";
        }
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
    const auto peer = server.peer().snapshot();
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
            label(ui, peer.attached ? peer.device : pairing.name, {qr_left + 18, top + 61, 220, 32}, 17, text, true);
            if (pairing.state == State::pending) {
                label(ui, std::string(peer.outgoing ? "等待对方确认 · " : "核对双方验证码 · ") + std::to_string(pairing.remaining_seconds) + " 秒",
                      {qr_left + 18, top + 99, 220, 24}, 12, muted);
                label(ui, peer.outgoing ? peer.verification : pairing.verification, {qr_left + 18, top + 130, 220, 42}, 27, online, true);
                auto permissions = snapshot.permissions;
                if (!peer.outgoing) {
                    ui.checkbox(&permissions.download, "浏览下载", {qr_left + 18, top + 178, 104, 24});
                    ui.checkbox(&permissions.upload, "申请发送", {qr_left + 134, top + 178, 104, 24});
                    ui.checkbox(&permissions.text, "文本图片", {qr_left + 18, top + 207, 104, 24});
                    ui.checkbox(&permissions.cast, "屏幕观看", {qr_left + 134, top + 207, 104, 24});
                    if (permissions.download != snapshot.permissions.download || permissions.upload != snapshot.permissions.upload ||
                        permissions.text != snapshot.permissions.text || permissions.cast != snapshot.permissions.cast)
                        session.set_permissions(permissions);
                }
                if (!peer.outgoing && button(ui, "允许连接", {qr_left + 18, top + 252, 104, 34}, true)) {
                    session.decide_pairing(pairing.token, true);
                }
                if (button(ui, peer.outgoing ? "取消请求" : "拒绝", {qr_left + 134, top + 252, 104, 34})) {
                    if (peer.outgoing) session.disconnect(pairing.token);
                    else session.decide_pairing(pairing.token, false);
                }
            } else {
                const bool connected = peer.attached ? peer.online : pairing.online;
                label(ui, connected ? "在线 · 已配对" : "已配对 · 等待恢复", {qr_left + 18, top + 97, 220, 24}, 13, connected ? online : muted, true);
                label(ui, peer.attached ? "桌面互联 · 可双向投屏" : "浏览器连接", {qr_left + 18, top + 121, 220, 21}, 12, muted);
                const auto waiting = std::count_if(snapshot.offers.begin(), snapshot.offers.end(),
                    [](const tempshare::Offer& offer) { return offer.state == State::pending; });
                if (waiting && button(ui, std::to_string(waiting) + " 个文件待接收",
                                      {qr_left + 18, top + 143, 220, 36}, true)) {
                    view.page = View::Page::share;
                    window.wake();
                }
                if (snapshot.capture_ready) {
                    label(ui, "正在共享屏幕", {qr_left + 18, top + 187, 220, 27}, 13, online);
                    if (button(ui, "停止共享", {qr_left + 18, top + 219, 220, 33})) session.stop_cast();
                }
                if (button(ui, "离开设备", {qr_left + 18, top + 262, 103, 28})) view.confirm_leave = true;
                if (button(ui, snapshot.trust_enabled ? "设备信任" : "信任设置", {qr_left + 134, top + 262, 104, 28})) {
                    view.page = View::Page::devices;
                }
            }
        } else {
            if (networks.empty()) {
                label(ui, "没有可用的局域网地址", {qr_left + 18, top + 60, 220, 32}, 15, text, true);
                label(ui, "请连接与手机相同的网络", {qr_left + 18, top + 97, 220, 25}, 12, muted);
            } else {
                draw_qr(ui, *view.qr, {qr_left + 48, top + 36, 160, 160});
            }
            if (!networks.empty()) {
                if (networks.size() > 1) {
                    std::vector<std::string> choices;
                    for (const auto& network : networks) choices.push_back(server.url(network.address));
                    int index = static_cast<int>(std::distance(networks.begin(), current));
                    if (ui.menu(choices[static_cast<std::size_t>(index)], choices, &index,
                                {qr_left + 18, top + 202, 220, 30}, Style().font(13))) {
                        view.selected_address = networks[static_cast<std::size_t>(index)].address;
                        window.wake();
                    }
                } else {
                    ui.draw_text(url, {qr_left + 18, top + 202, 220, 30}, text, Style().center().font(13));
                }
                const auto adapter = current->interface_name.empty() ? (current->wireless ? "Wi-Fi" : "局域网")
                                                                    : current->interface_name;
                ui.draw_text(adapter, {qr_left + 18, top + 232, 220, 19}, muted, Style().center().font(11));
                label(ui, "连接码 " + snapshot.code, {qr_left + 18, top + 263, 114, 28}, 13, text, true);
                if (button(ui, "复制邀请链接", {qr_left + 140, top + 263, 98, 28})) {
                    view.status = window.try_set_clipboard(invitation) ? "邀请链接已复制" : "无法写入剪贴板";
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
            if (button(ui, snapshot.unlimited ? "不限时 · 设置" : "续期 · " + std::to_string((snapshot.remaining_seconds + 59) / 60) + " 分",
                       {qr_left + 18, top + 301, 101, 28})) {
                if (snapshot.unlimited) view.page = View::Page::settings;
                else if (session.extend()) view.status = "已从现在起重新计时 " + std::to_string(snapshot.lifetime_seconds / 60) + " 分钟";
            }
            if (button(ui, "结束会话", {qr_left + 132, top + 301, 106, 28})) {
                if (active_requests || snapshot.capture_ready || snapshot.pairing || !snapshot.messages.empty()) {
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
    card(ui, {x, 142, transfer_width, 156});
    label(ui, "共享下载清单（对方选择保存）", {x + 18, 155, transfer_width - 36, 30}, 16, text, true);
#ifdef _WIN32
    label(ui, "拖入文件，或选择要分享的文件", {x + 18, 187, transfer_width - 36, 25}, 13, muted);
#else
    label(ui, "选择文件或在下方输入文件路径", {x + 18, 187, transfer_width - 36, 25}, 13, muted);
#endif
    if (button(ui, snapshot.active ? "选择文件" : "新建会话", {x + 18, 217, 92, 32}, true)) {
        if (!snapshot.active) { session.start(); return; }
        share_paths(session, view, pick_paths(view, false));
    }
    if (!view.folder_job.valid()) {
        if (button(ui, "分享文件夹", {x + 119, 217, 101, 32}, false, 0, snapshot.active)) share_folder(session, window, view, snapshot, false);
        if (button(ui, "打包 ZIP", {x + 229, 217, 86, 32}, false, 0, snapshot.active)) share_folder(session, window, view, snapshot, true);
    } else if (view.folder_job.valid()) {
        if (button(ui, "取消目录任务", {x + 119, 217, 135, 32})) *view.folder_cancel = true;
        const auto scanned = view.folder_scanned->load();
        if (scanned && transfer_width > 355) {
            label(ui, "已读取 " + std::to_string(scanned) + " 项", {x + 262, 217, transfer_width - 275, 32}, 12, muted);
        }
    }
    ui.input(&view.path, {x + 18, 261, transfer_width - 129, 27}, Style().hint("或粘贴文件/文件夹路径"));
    if (button(ui, "添加共享", {x + transfer_width - 103, 261, 85, 27}) && !view.path.empty())
        share_selection(session, view, {std::filesystem::u8path(view.path)});
    constexpr float list_y = 315.0f;
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
    std::vector<std::string> visible;
    std::uint64_t visible_bytes = 0;
    for (const auto* entry : pending) {
        const auto& candidate = *entry;
        if (candidate.state == State::pending) {
            visible.push_back(candidate.id);
            visible_bytes += candidate.size;
        }
    }
    if (visible.size() > 1 && button(ui, "接收全部 " + std::to_string(visible.size()),
                                     {x + 104, list_y, 92, 28}, true)) {
        view.batch_ids = visible; view.batch_accept = true; view.batch_open = true;
    }
    if (visible.size() > 1 && transfer_width >= 405 && button(ui, "拒绝全部", {x + 200, list_y, 73, 28})) {
        view.batch_ids = visible; view.batch_accept = false; view.batch_open = true;
    }
    if (visible.size() > 1 && transfer_width >= 400) {
        label(ui, readable_size(visible_bytes), {x + 280, list_y, 68, 24}, 12, muted);
    }
    float row = list_y + 34;
    if (pending.empty()) {
        label(ui, "暂无接收请求", {x, row, transfer_width, 26}, 13, muted);
        row += 36;
    } else {
        const float request_height = std::min(static_cast<float>(pending.size()) * 48,
            std::max(96.0f, (bottom - row - 100) * .45f));
        ui.scroll({x, row, transfer_width, request_height}, static_cast<int>(pending.size()), 48,
          [&](int index, const Rect& box) {
            const float row = box.y;
            const auto& offer = *pending[static_cast<std::size_t>(index)];
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
                if (button(ui, "取消", {x + transfer_width - 64, row + 7, 52, 28}, false, id + 2)) session.cancel_offer(offer.id);
            } else if (offer.state == State::uploading) {
                if (button(ui, "取消", {x + transfer_width - 64, row + 7, 52, 28}, false, id + 2)) session.cancel_offer(offer.id);
            }
        });
        row += request_height + 8;
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
            const float actions = 137.0f;
            const bool available = file.folder || tempshare::SharedSource(file.path).version() == file.source;
            label(ui, file.name + "  ·  " + (file.folder ? "可浏览的文件夹" : readable_size(file.size)),
                  {box.x + name_inset, box.y + 2, box.w - actions - name_inset, 34}, 13, available ? text : Color{235, 100, 100});
            if (!file.folder && button(ui, available ? "重发" : "刷新", {box.x + box.w - 128, box.y + 5, 50, 28}, false,
                                      std::hash<std::string>{}("reload" + file.id))) {
                if (session.share(file.path)) { session.unshare(file.id); view.status = "文件已重新发布，旧链接已失效"; }
                else view.status = "文件不可读取或超过限制";
            }
            if (file.folder && !view.folder_job.valid() && button(ui, "刷新", {box.x + box.w - 128, box.y + 5, 50, 28},
                       false, std::hash<std::string>{}("refresh" + file.id))) {
                share_folder(session, window, view, snapshot, false, file.path, file.id);
            }
            if (button(ui, "移除", {box.x + box.w - 71, box.y + 5, 50, 28}, false,
                       std::hash<std::string>{}(file.id))) session.unshare(file.id);
        });
}

void render_remote_files(Ui& ui, tempshare::HttpServer& http, tempshare::Session& session,
                         View& view, float x, float width, float bottom) {
    const auto remote = http.peer().snapshot();
    card(ui, {x, 148, width, 110});
    label(ui, "来自 " + remote.device, {x + 16, 158, width - 32, 28}, 16, text, true);
    label(ui, remote.transferring ? "正在传输 " + readable_size(remote.transferred) + " / " + readable_size(remote.total) :
          remote.transfer_status.empty() ? remote.status : remote.transfer_status, {x + 16, 191, width - 32, 24}, 12, muted);
    if (remote.transferring && button(ui, "取消传输", {x + 16, 222, 88, 27})) http.peer().cancel_transfer();
    if (!remote.transferring && button(ui, "发送文件给对方", {x + 16, 222, 138, 27}, true)) {
        const auto files = pick_paths(view, false);
        if (!files.empty() && !http.peer().send(files)) view.status = "无法发送，请检查连接、上传权限和单文件1GiB限制";
    }
    if (button(ui, "浏览对方文件夹", {x + width - 161, 222, 145, 27})) {
        if (!open_url(http.url("127.0.0.1") + "peer#" + http.peer().browser_key() + ":share"))
            view.status = "无法打开文件浏览器";
    }
    if (remote.files.empty()) { label(ui, "对方尚未共享文件，或下载权限已关闭", {x, 282, width, 30}, 13, muted); return; }
    ui.scroll({x, 278, width, std::max(50.0f, bottom - 278)}, static_cast<int>(remote.files.size()), 60,
        [&](int index, const Rect& box) {
            const auto& file = remote.files[static_cast<std::size_t>(index)];
            card(ui, {box.x, box.y, box.w - 10, 54});
            label(ui, file.name, {box.x + 12, box.y + 3, box.w - 118, 25}, 14, text, true);
            label(ui, file.folder ? "可浏览的文件夹" : readable_size(file.size),
                  {box.x + 12, box.y + 29, box.w - 118, 19}, 12, muted);
            if (!file.folder && button(ui, "保存", {box.x + box.w - 88, box.y + 12, 65, 30}, false,
                                      std::hash<std::string>{}(file.id))) {
                if (!http.peer().download(file)) view.status = "当前有下载任务，或连接/下载权限不可用";
                else view.status = "正在保存到 " + session.snapshot(View::Page::home).directory.u8string();
            }
        });
}


void render_nearby(Ui& ui, View& view, tempshare::Discovery& discovery, bool nearby_available, float x, float width,
                   float bottom, const std::function<void(const std::string&)>& join,
                   const std::function<bool(const tempshare::Peer&)>& connected) {
    const auto peers = discovery.peers();
    ui.input(&view.nearby_address, {x, 106, width - 94, 31}, Style().hint("粘贴局域网地址、主机名或完整邀请链接"));
    if (button(ui, "连接设备", {x + width - 85, 106, 85, 31})) join(view.nearby_address);
    if (button(ui, "刷新", {x, 151, 62, 26})) {
        discovery.refresh();
        view.status = nearby_available ? "已重新发送局域网发现请求" : "没有可用的局域网地址";
    }
    if (peers.empty()) label(ui, discovery.status(),
                            {x + 73, 149, width - 75, 32}, 13, muted);
    ui.scroll({x, 183, width, std::max(56.0f, bottom - 183)}, static_cast<int>(peers.size()), 56,
      [&](int index, const Rect& row) {
        const float y = row.y;
        const auto& peer = peers[static_cast<std::size_t>(index)];
        card(ui, {x, y, width - 10, 50});
        label(ui, peer.name, {x + 14, y + 3, width - 120, 24}, 15, text, true);
        label(ui, peer.platform + " · " + peer.address + (peer.id.empty() ? "" : " · " + peer.id.substr(0, 6)),
              {x + 14, y + 26, width - 120, 17}, 12, muted);
        ui.tooltip({x + 14, y + 3, width - 120, 42}, peer.name + "\n" + peer.url() +
                   (peer.id.empty() ? "" : "\n设备标识 " + peer.id));
        const Rect action{x + width - 85, y + 9, 70, 32};
        if (button(ui, connected(peer) ? "查看" : "连接", action, false, std::hash<std::string>{}(peer.url()))) join(peer.url());
    });
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
    ui.tooltip({x + 18, 151, width - 36, 28}, snapshot.directory.u8string());
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

    label(ui, "接收目录为本机全局设置。", {x, 280, width, 24}, 13, muted);
    label(ui, "设备授权请打开侧栏的「设备与权限」。", {x, 304, width, 20}, 12, muted);
}

bool paired(const tempshare::Snapshot& snapshot) {
    return snapshot.pairing && snapshot.pairing->state == State::approved;
}

void render_devices(Ui& ui, tempshare::Session& session, const tempshare::Snapshot& snapshot,
                    View& view, float x, float width, float bottom) {
    card(ui, {x, 106, width, 185});
    label(ui, "当前设备权限（不影响其他设备）", {x + 18, 119, width - 36, 28}, 16, text, true);
    auto permissions = snapshot.permissions;
    ui.checkbox(&permissions.download, "允许对方下载共享文件", {x + 18, 155, width - 36, 25});
    ui.checkbox(&permissions.upload, "允许对方申请发送文件", {x + 18, 185, width - 36, 25});
    ui.checkbox(&permissions.text, "允许文本和图片（关闭后保留已有记录）", {x + 18, 215, width - 36, 25});
    ui.checkbox(&permissions.cast, "允许共享与观看屏幕", {x + 18, 245, width - 36, 25});
    if (permissions.download != snapshot.permissions.download || permissions.upload != snapshot.permissions.upload ||
        permissions.text != snapshot.permissions.text || permissions.cast != snapshot.permissions.cast) {
        if ((!permissions.upload && snapshot.permissions.upload && snapshot.pending_transfers) ||
            (!permissions.cast && snapshot.permissions.cast && snapshot.casting)) view.permissions_pending = permissions;
        else session.set_permissions(permissions);
    }
    card(ui, {x, 306, width, 104});
    if (!snapshot.trust_enabled) {
        label(ui, "持久信任需要配置 HTTPS 和可写的本机配置目录。",
              {x + 18, 323, width - 36, 55}, 14, muted);
        return;
    }
    if (paired(snapshot)) {
        label(ui, "当前设备 · " + snapshot.pairing->name, {x + 18, 316, width - 36, 32}, 16, text, true);
        const bool known = !snapshot.pairing->trusted_key.empty();
        if (button(ui, known ? "更新信任权限" : "记住此设备", {x + 18, 358, 121, 34}, true)) {
            view.status = session.remember_device() ? "已保存设备和当前会话权限" : "无法保存设备信任";
        }
        label(ui, "保存上方当前授权", {x + 154, 360, width - 172, 28}, 12, muted);
    } else {
        label(ui, "配对设备后，可在这里主动保存信任和授权范围。",
              {x + 18, 323, width - 36, 30}, 14, muted);
    }
    label(ui, "已记住的设备", {x, 423, width, 30}, 16, text, true);
    ui.scroll({x, 461, width, std::max(54.0f, bottom - 461)}, static_cast<int>(snapshot.trusted.size()), 54,
      [&](int index, const Rect& row) {
        const auto& device = snapshot.trusted[static_cast<std::size_t>(index)];
        const float y = row.y;
        card(ui, {x, y, width - 10, 48});
        label(ui, device.name, {x + 14, y + 3, width - 126, 23}, 14, text, true);
        std::string policy;
        if (device.permissions.download) policy += "下载  ";
        if (device.permissions.upload) policy += "申请上传  ";
        if (device.permissions.text) policy += "文本  ";
        if (device.permissions.cast) policy += "屏幕";
        label(ui, policy.empty() ? "仅建立连接" : policy, {x + 14, y + 26, width - 126, 17}, 12, muted);
        if (button(ui, "撤销信任", {x + width - 100, y + 8, 85, 32}, false, std::hash<std::string>{}(device.key))) {
            view.status = session.revoke_device(device.key) ? "设备信任已撤销" : "无法删除设备信任";
        }
    });
}

void render_sync(Ui& ui, aino::Window& window, tempshare::Session& session,
                 const tempshare::Snapshot& snapshot, View& view, float x, float width, float bottom, const tempshare::HttpServer& http) {
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
        view.confirm_clear = true;
    }
    ui.input(&view.message_search, {x, 350, width - 96, 29}, Style().hint("搜索本会话消息"));
    if (button(ui, "导出", {x + width - 86, 350, 86, 29})) {
        const auto target = snapshot.directory / ("TempShare-消息-" + tempshare::random_token().substr(0, 8) + ".txt");
        std::error_code error;
        std::filesystem::create_directories(snapshot.directory, error);
        std::ofstream output(target, std::ios::binary);
        for (const auto& entry : snapshot.messages) output << (entry.from_desktop ? "本机" : "对方") << " · " << readable_time(entry.sent_at) << "\n" << entry.text << "\n\n";
        output.close();
        view.status = output ? "已导出到 " + target.u8string() : "导出失败，请检查接收目录";
    }
    std::vector<const tempshare::Message*> messages;
    for (auto it = snapshot.messages.rbegin(); it != snapshot.messages.rend(); ++it)
        if (view.message_search.empty() || it->text.find(view.message_search) != std::string::npos) messages.push_back(&*it);
    if (messages.empty()) label(ui, view.message_search.empty() ? "暂无消息，发送内容后会显示在这里" : "没有匹配的消息", {x, 396, width, 28}, 13, muted);
    ui.scroll({x, 393, width, std::max(80.0f, bottom - 393)}, static_cast<int>(messages.size()), 92,
      [&](int index, const Rect& row) {
        const auto& entry = *messages[static_cast<std::size_t>(index)];
        const float y = row.y;
        card(ui, {x, y, width - 10, 84});
        label(ui, std::string(entry.from_desktop ? "本机" : "对方") + " · " + readable_time(entry.sent_at),
              {x + 13, y + 4, width - 32, 22}, 12, muted);
        ui.draw_text(entry.attachment.empty() ? entry.text : "图片 · " + entry.text,
                     {x + 13, y + 28, width - 108, 46}, text, Style().font(13).top().wrap_text());
        if (button(ui, entry.attachment.empty() ? "查看" : "打开", {x + width - 72, y + 22, 57, 28}, false, entry.sequence)) {
            if (entry.attachment.empty()) { view.message_detail = entry; view.detail_text = entry.text; }
            else if (entry.remote_attachment) {
                open_url(http.url("127.0.0.1") + "peer#" + http.peer().browser_key() + ":sync");
            } else {
                const auto find = [&](const std::vector<tempshare::File>& files) {
                    const auto file = std::find_if(files.begin(), files.end(), [&](const tempshare::File& file) { return file.id == entry.attachment; });
                    return file == files.end() ? std::filesystem::path{} : file->path;
                };
                const auto all = session.snapshot(View::Page::all);
                const auto path = entry.from_desktop ? find(all.shared) : find(all.received);
                if (path.empty() || !open_folder(path)) view.status = "附件不可用，请查看文件页或接收记录";
            }
        }
    });
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
    if (!snapshot.capture_ready && snapshot.permissions.cast &&
        (!server.peer().snapshot().attached || server.peer().snapshot().permissions.cast) &&
        button(ui, snapshot.casting ? "重新打开来源选择" : "共享我的屏幕",
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
    const auto remote = server.peer().snapshot();
    if (remote.attached && !remote.permissions.cast)
        label(ui, "对方关闭了屏幕权限", {x + 18, 254, width - 36, 24}, 12, muted);
    if (remote.attached) {
        card(ui, {x, 316, width, 152});
        label(ui, "观看对方 · " + remote.device, {x + 18, 331, width - 36, 30}, 16, text, true);
        label(ui, remote.capture_ready ? "对方屏幕已就绪，可开始观看" : "等待对方点击共享我的屏幕",
              {x + 18, 369, width - 36, 26}, 13, muted);
        if (button(ui, "打开观看窗口", {x + 18, 412, 142, 34}, remote.capture_ready)) {
            if (!open_url(server.url("127.0.0.1") + "peer#" + server.peer().browser_key() + ":cast"))
                view.status = "无法打开观看窗口";
        }
        label(ui, remote.status, {x, 486, width, 28}, 12, muted);
    }
}

void render_home(Ui& ui, View& view, const std::vector<tempshare::Peer>& peers,
                 float x, float width, float nearby_width, const std::function<void(const std::string&)>& join,
                 const std::string& discovery_status, const std::function<bool(const tempshare::Peer&)>& connected) {
    struct Feature {
        const char* title;
        const char* detail;
        const char* action;
        View::Page page;
        SystemIcon icon;
        Color top, bottom, tint;
    };
    const std::array<Feature, 3> features{{
        {"文件分享", "传输文件和文件夹", "打开文件页", View::Page::share, SystemIcon::Folder,
         {45, 65, 138}, {29, 38, 91}, {95, 186, 255}},
        {"内容同步", "发送文本、链接或图片", "打开同步页", View::Page::sync, SystemIcon::Text,
         {69, 55, 135}, {42, 35, 89}, {184, 155, 255}},
        {"屏幕投递", "共享屏幕或窗口", "打开投屏页", View::Page::cast, SystemIcon::Play,
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
        label(ui, discovery_status, {x + 64, 357, nearby_width - 80, 29}, 13, text, true);
        label(ui, "电脑可在附近设备页粘贴地址；手机可扫码加入", {x + 64, 386, nearby_width - 80, 22}, 12, muted);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(peers.size(), 4); ++i) {
        const auto& peer = peers[i];
        const float y = 348 + static_cast<float>(i) * 62;
        card(ui, {x, y, nearby_width, 54});
        ui.system_icon(SystemIcon::Network, {x + 15, y + 13, 28, 28}, accent);
        label(ui, peer.name, {x + 55, y + 4, nearby_width - 146, 24}, 14, text, true);
        label(ui, peer.platform + " · " + peer.address, {x + 55, y + 28, nearby_width - 146, 19}, 12, muted);
        if (button(ui, connected(peer) ? "查看" : "连接", {x + nearby_width - 75, y + 12, 61, 30}, false,
                   std::hash<std::string>{}(peer.url()))) join(peer.url());
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
        : session(inbox, repaint, std::chrono::minutes(load_session_minutes(config)),
                   options.cert.empty() || config.empty() ? std::filesystem::path{} : config / "trusted",
                  config.empty() ? std::filesystem::path{} :
                      config / (index ? "received-history-" + std::to_string(index + 1) : "received-history")),
          http(session, options.cert, options.key, repaint) {
        const unsigned preferred = options.port + static_cast<unsigned>(index);
        const auto port = static_cast<unsigned short>(preferred <= 65535 ? preferred : 0);
        if (!http.start(index ? 0 : port, index != 0)) throw std::runtime_error("无法启动局域网服务，请检查端口与证书。");
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
    const auto repaint = [&window] { window.wake(); };
#ifdef _WIN32
    DesktopIntegration desktop(dropped, repaint);
#endif
    const auto config_path = configuration_directory();
    int appearance = load_appearance(config_path);
    bool system_dark = system_dark_mode().value_or(true);
    auto next_appearance_check = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<bool> applied_dark;
    const auto inbox = receive_directory(options, config_path);
    auto applied_directory = inbox;
#ifdef _WIN32
    desktop.inbox = inbox;
#endif
    bool close_dialog_open = false;
    window.set_close_handler([&] { close_dialog_open = true; });
    constexpr std::size_t connection_limit = 4;
    std::array<std::shared_ptr<Connection>, connection_limit> connections;
    connections[0] = std::make_shared<Connection>(options, inbox, config_path, repaint, 0);
    for (std::size_t index = 1; index < connections.size(); ++index)
        connections[index] = std::make_shared<Connection>(options, inbox, config_path, repaint, index);
    const auto primary = connections.front();
    std::vector<tempshare::HttpServer*> endpoints;
    for (const auto& item : connections) endpoints.push_back(&item->http);
    primary->http.configure_gateway(endpoints);
    std::string device_identity;
    std::string nickname;
    { std::ifstream input(config_path / "device-id"); input >> device_identity; }
    if (device_identity.size() != 32 || device_identity.find_first_not_of("0123456789abcdef") != std::string::npos) {
        device_identity = tempshare::random_token();
        std::error_code error; std::filesystem::create_directories(config_path, error);
        std::ofstream(config_path / "device-id") << device_identity;
    }
    { std::ifstream input(config_path / "device-name"); std::getline(input, nickname); }
    if (!tempshare::valid_filename(nickname) || nickname.size() > 80) nickname = tempshare::device_hostname();
    for (const auto& item : connections) item->http.peer().set_identity(device_identity, nickname);
    tempshare::Discovery discovery(primary->http.port(), !options.cert.empty(),
                                   [primary] { return primary->http.networks(); }, repaint, device_identity, nickname);
    int selected_connection = 0;
    bool requests_open = false;
    const auto connected_slot = [&](const tempshare::Peer& peer) {
        for (std::size_t slot = 0; slot < connections.size(); ++slot) {
            const auto link = connections[slot]->http.peer().snapshot();
            if (!link.attached) continue;
            if (!peer.id.empty() && link.device_id == peer.id) return static_cast<int>(slot);
            auto target = tempshare::parse_lan_endpoint(link.address);
            if (target && target->port == peer.port && target->secure == peer.secure &&
                (target->host == peer.address || std::find(peer.addresses.begin(), peer.addresses.end(), target->host) != peer.addresses.end()))
                return static_cast<int>(slot);
        }
        return -1;
    };
    const auto is_connected = [&](const tempshare::Peer& peer) { return connected_slot(peer) >= 0; };
    const auto join_peer = [&](const std::string& address) {
        std::vector<std::string> alternatives;
        std::string expected_device;
        for (auto peer : discovery.peers()) if (peer.url() == address) {
            const auto existing = connected_slot(peer);
            if (existing >= 0) {
                selected_connection = existing;
                auto& selected = connections[static_cast<std::size_t>(existing)]->view;
                selected.page = View::Page::home; selected.connection_open = true;
                selected.status = "此设备已连接"; window.wake(); return;
            }
            expected_device = peer.id;
            for (const auto& candidate : peer.addresses) {
                peer.address = candidate;
                if (alternatives.size() < 16) alternatives.push_back(peer.url());
            }
            break;
        }
        auto slot = static_cast<std::size_t>(selected_connection);
        if (connections[slot]->session.snapshot(View::Page::home).pairing || connections[slot]->http.peer().snapshot().attached) {
            slot = 0;
            while (slot < connections.size() && (connections[slot]->session.snapshot(View::Page::home).pairing ||
                   connections[slot]->http.peer().snapshot().attached)) ++slot;
        }
        if (slot == connections.size()) {
            connections[static_cast<std::size_t>(selected_connection)]->view.status = "当前设备已满，请先离开一个连接";
        } else if (connections[slot]->http.peer().join(address, alternatives, expected_device)) {
            selected_connection = static_cast<int>(slot);
            connections[slot]->view.page = View::Page::home;
            connections[slot]->view.connection_open = true;
        } else connections[slot]->view.status = "请输入局域网地址或完整邀请链接；HTTPS需要启用TLS的构建";
        window.wake();
    };

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
#ifdef _WIN32
            if (desktop.clicked_connection >= 0 && desktop.clicked_connection < static_cast<int>(connections.size())) {
                auto& target = connections[static_cast<std::size_t>(desktop.clicked_connection)];
                if (target && target->session.invitation_active(desktop.clicked_invitation)) {
                    selected_connection = desktop.clicked_connection;
                    if (desktop.clicked_sync) target->view.page = View::Page::sync;
                    else target->view.connection_open = true;
                }
                desktop.clicked_connection = -1;
            }
#endif
            const auto connection = connections[static_cast<std::size_t>(selected_connection)];
            auto& session = connection->session;
            auto& http = connection->http;
            auto& view = connection->view;
            const auto current_directory = session.snapshot(View::Page::home).directory;
            if (current_directory != applied_directory) {
                for (const auto& item : connections) item->session.set_directory(current_directory);
                applied_directory = current_directory;
            }
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
            desktop.inbox = session.notification_state().directory;
            for (std::size_t i = 0; i < connections.size(); ++i) {
                const auto& item = connections[i];
                if (!item) continue;
                const auto slot = static_cast<int>(i);
                const auto notification = item->session.notification_state();
                const auto invitation = item->session.snapshot(View::Page::home).invitation;
                auto& last = item->last_notice;
                if (notification.pairing != last.pairing && !notification.pairing.empty()) {
                    if (desktop.notify(L"TempShare · 连接请求", L"点击核对对方设备上的验证码。", slot, invitation)) last.pairing = notification.pairing;
                }
                if (notification.received != last.received && !notification.received.empty()) {
                    if (desktop.notify(L"TempShare · 文件已接收", L"文件已保存，点击打开 TempShare。", slot, invitation)) last.received = notification.received;
                }
                if (notification.offer != last.offer && !notification.offer.empty()) {
                    if (desktop.notify(L"TempShare · 文件接收请求", L"点击查看并确认待接收文件。", slot, invitation)) last.offer = notification.offer;
                }
                if (notification.incoming_messages != last.incoming_messages) {
                    if (desktop.notify(L"TempShare · 新消息", L"点击打开内容同步。", slot, invitation, true)) last.incoming_messages = notification.incoming_messages;
                }
                if (notification.pairing.empty()) last.pairing.clear();
                if (notification.offer.empty()) last.offer.clear();
                if (notification.received.empty()) last.received.clear();
            }
            desktop.pump_notifications();
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
            const bool auto_hide = width < 1100 && view.page != View::Page::home &&
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
                view.confirm_end = false;
            }
            const std::array<const char*, 8> titles{
                "首页", "文件分享", "内容同步", "屏幕投递", "附近设备", "接收记录", "设备与权限", "设置"
            };
            constexpr std::array<View::Page, 8> pages{View::Page::home, View::Page::share, View::Page::sync, View::Page::cast,
                View::Page::nearby, View::Page::received, View::Page::devices, View::Page::settings};
            constexpr std::array<SystemIcon, 8> icons{SystemIcon::Card, SystemIcon::Folder, SystemIcon::Text,
                SystemIcon::Play, SystemIcon::Network, SystemIcon::Download, SystemIcon::People, SystemIcon::Settings};
            constexpr float nav_top = 81, nav_step = 40, nav_height = 34, group_gap = 16;
            constexpr std::size_t group_start = 4;
            const auto incoming_messages = session.notification_state().incoming_messages;
            if (view.page == View::Page::sync) view.last_read_messages = incoming_messages;
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
                if (page == View::Page::sync && incoming_messages > view.last_read_messages) {
                    ui.fill_circle(side_width - 25, y + 17, 3.5f, accent);
                }
            }
            constexpr float divider = nav_top + nav_step * group_start + (group_gap + nav_height - nav_step) / 2;
            ui.fill_rect({23, divider, side_width - 46, 1}, border);
            std::vector<std::string> devices;
            std::vector<int> device_slots;
            for (const auto& item : connections) {
                if (!item) break;
                const auto state = item->session.snapshot(View::Page::home);
                const auto slot = static_cast<int>(&item - connections.data());
                const auto remote = item->http.peer().snapshot();
                if (slot != selected_connection && !state.pairing && !remote.attached) continue;
                std::string name = state.pairing ? (state.pairing->state == State::pending ? "待确认 · " : "已配对 · ") : "邀请设备 · ";
                name += remote.attached ? remote.device : state.pairing ? state.pairing->name : "本机";
                devices.push_back(std::move(name));
                device_slots.push_back(slot);
            }
            int selected_device = static_cast<int>(std::find(device_slots.begin(), device_slots.end(), selected_connection) - device_slots.begin());
            if (ui.menu(devices[static_cast<std::size_t>(selected_device)], devices, &selected_device,
                        {navigation_inset, 447, navigation_width, 29}, Style().font(12))) {
                selected_connection = device_slots[static_cast<std::size_t>(selected_device)];
                window.wake();
            }
            if (button(ui, "＋ 邀请设备", {navigation_inset, 486, navigation_width, 29})) {
                std::size_t slot = 0;
                while (slot < connections.size() && (connections[slot]->session.snapshot(View::Page::home).pairing ||
                       connections[slot]->http.peer().snapshot().attached)) ++slot;
                if (slot == connections.size()) view.status = "最多同时使用 4 个连接，可先结束一个";
                else {
                    if (!connections[slot]->session.is_active()) connections[slot]->session.start();
                    connections[slot]->view.page = View::Page::home;
                    connections[slot]->view.connection_open = true;
                    selected_connection = static_cast<int>(slot);
                    window.wake();
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
            std::vector<std::string> requests;
            std::size_t request_count = 0;
            for (const auto& item : connections) {
                const auto summary = item->session.snapshot(View::Page::home);
                std::string request;
                if (summary.pairing && summary.pairing->state == State::pending && !item->http.peer().snapshot().outgoing)
                    request = summary.pairing->name + " · 请求连接";
                else if (!summary.offers.empty()) request = summary.pairing ? summary.pairing->name + " · 有文件待接收" : "文件请求";
                if (!request.empty()) ++request_count;
                requests.push_back(std::move(request));
            }
            if (button(ui, "待处理 " + std::to_string(request_count), {navigation_inset, 526, navigation_width, 28})) requests_open = true;
            if ((!snapshot.downloads.empty() || !view.failed_files.empty()) &&
                button(ui, "传输活动与失败", {navigation_inset, 562, navigation_width, 28})) view.downloads_open = true;
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
                const float diagnostic_y = page == View::Page::home ? std::min(650.0f, height - 36.0f) : 463.0f;
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
                if (width < 1100 && page != View::Page::home &&
                    button(ui, "收起连接栏", {rail_left + 132, diagnostic_y, 106, 25})) view.connection_open = false;
            }
            if (page != view.page) snapshot = session.snapshot(view.page);
            const auto index = static_cast<std::size_t>(std::find(pages.begin(), pages.end(), view.page) - pages.begin());
            label(ui, view.page == View::Page::home ? "你好，TempShare" : titles[index],
                  {x, 24, content_width, 50}, 29, text, true);
            if (view.page == View::Page::home) {
                label(ui, "选择一种方式开始分享", {x, 69, content_width, 22}, 13, muted);
            } else if (view.page == View::Page::settings) {
                label(ui, "本机设置 · 应用到全部会话", {x, 77, content_width, 20}, 12, muted);
            } else {
                const auto remote = http.peer().snapshot();
                const auto target = remote.attached ? remote.device : snapshot.pairing ? snapshot.pairing->name : std::string{};
                label(ui, target.empty() ? "邀请设备或在附近设备中发起连接" : "连接对象 · " + target,
                      {x, 77, content_width, 20}, 12, muted);
                if (!target.empty()) ui.tooltip({x, 77, content_width, 20}, "当前设备 · " + target +
                    (remote.attached ? "\n" + remote.address + " · " + remote.device_id.substr(0, 8) : ""));
            }

            switch (view.page) {
            case View::Page::all: break;
            case View::Page::home:
                render_home(ui, view, discovery.peers(), x, width - x - 28, content_width, join_peer, discovery.status(), is_connected);
                if (page != view.page) window.wake();
                break;
            case View::Page::share:
                if (http.peer().snapshot().attached) {
                    static const std::vector<std::string> directions{"本机共享与接收请求", "对方共享文件"};
                    int direction = view.remote_files ? 1 : 0;
                    if (ui.segmented(directions, &direction, {x, 97, content_width, 32}, Style().font(12)))
                        view.remote_files = direction == 1;
                    if (view.remote_files) render_remote_files(ui, http, session, view, x, content_width, height - 53);
                    else {
                        render_share(ui, window, session, snapshot, view, x, content_width, height - 53);
                    }
                } else render_share(ui, window, session, snapshot, view, x, content_width, height - 53);
                break;
            case View::Page::sync: render_sync(ui, window, session, snapshot, view, x, content_width, height - 53, http); break;
            case View::Page::cast: render_cast(ui, session, http, snapshot, view, x, content_width); break;
            case View::Page::nearby: render_nearby(ui, view, discovery, !http.networks().empty(), x, content_width, height - 53, join_peer, is_connected); break;
            case View::Page::received: render_received(ui, window, session, view, snapshot, x, content_width, height - 53); break;
            case View::Page::devices: render_devices(ui, session, snapshot, view, x, content_width, height - 53); break;
            case View::Page::settings:
                render_settings(ui, view, session, snapshot, config_path, x, content_width);
                card(ui, {x, 327, content_width, 133});
                label(ui, "会话时长", {x + 18, 338, content_width - 36, 28}, 16, text, true);
                int duration_choice = 0;
                for (std::size_t i = 0; i < session_durations.size(); ++i)
                    if (snapshot.lifetime_seconds == static_cast<std::uint64_t>(session_durations[i]) * 60) duration_choice = static_cast<int>(i);
                if (ui.menu(session_duration_names[static_cast<std::size_t>(duration_choice)], session_duration_names, &duration_choice,
                            {x + 18, 375, content_width - 36, 30}, Style().font(13))) {
                    const int minutes = session_durations[static_cast<std::size_t>(duration_choice)];
                    for (const auto& item : connections) item->session.set_lifetime(std::chrono::minutes(minutes));
                    std::ofstream output(config_path / "session-duration"); output << minutes; output.close();
                    view.status = output ? "已保存会话时长，当前全部会话从现在起应用" : "会话时长已应用，本机偏好保存失败";
                }
                label(ui, "应用到全部会话；不限时仍需配对，可随时离开或结束。", {x + 18, 418, content_width - 36, 26}, 12, muted);
                if (view.device_name.empty()) view.device_name = nickname;
                ui.input(&view.device_name, {x, 488, content_width - 102, 32}, Style().hint("本机设备名称"));
                if (button(ui, "保存名称", {x + content_width - 92, 488, 92, 32})) {
                    if (tempshare::valid_filename(view.device_name) && view.device_name.size() <= 80) {
                        std::ofstream output(config_path / "device-name"); output << view.device_name; output.close();
                        if (output) {
                            nickname = view.device_name; discovery.set_name(nickname);
                            for (const auto& item : connections) item->http.peer().set_identity(device_identity, nickname);
                            view.status = "设备名称已保存";
                        }
                        else view.status = "无法保存设备名称";
                    } else view.status = "请输入有效设备名称（最多80字节）";
                }
#ifdef _WIN32
                label(ui, "TempShare " TEMPSHARE_VERSION, {x, 585, 123, 32}, 13, muted);
                if (button(ui, "检查更新", {x + 128, 585, 86, 32}) && !desktop.updates()) {
                    view.status = "无法启动更新检查";
                }
#endif
                break;
            }
            label(ui, view.status, {x, height - 43, content_width, 27}, 12, muted);
            if (view.batch_open) {
                const Rect box{(width - 500) / 2, (height - 420) / 2, 500, 420};
                std::uint64_t total = 0;
                std::vector<const tempshare::Offer*> entries;
                const auto current = session.snapshot(View::Page::share);
                for (const auto& offer : current.offers) if (std::find(view.batch_ids.begin(), view.batch_ids.end(), offer.id) != view.batch_ids.end()) {
                    entries.push_back(&offer); total += offer.size;
                }
                std::error_code error; const auto disk = std::filesystem::space(current.directory, error);
                const auto body = std::to_string(entries.size()) + " 个文件 · " + readable_size(total) + "\n保存到 " +
                    current.directory.u8string() + (error ? "\n目录将在接收时创建" : "\n可用空间 " + readable_size(disk.available));
                if (ui.dialog(view.batch_accept ? "确认接收整批文件" : "确认拒绝整批文件", body, &view.batch_open, box)) {
                    ui.scroll({box.x + 20, box.y + 150, box.w - 40, 200}, static_cast<int>(entries.size()), 30,
                        [&](int index, const Rect& row) { label(ui, entries[static_cast<std::size_t>(index)]->name, row, 13); });
                    if (button(ui, view.batch_accept ? "确认接收" : "确认拒绝", {box.x + 20, box.bottom() - 49, 120, 30}, true)) {
                        const auto count = session.decide_offers(view.batch_ids, view.batch_accept);
                        view.status = "已处理 " + std::to_string(count) + " 个请求"; view.batch_open = false;
                    }
                    if (button(ui, "取消", {box.right() - 110, box.bottom() - 49, 90, 30})) view.batch_open = false;
                }
            }
            if (view.downloads_open) {
                const Rect box{(width - 500) / 2, (height - 400) / 2, 500, 400};
                if (ui.dialog("对方下载活动", "显示本机传输结果；浏览器是否保存到磁盘由对方确认。", &view.downloads_open, box)) {
                    const auto current = session.snapshot(View::Page::share);
                    ui.scroll({box.x + 20, box.y + 85, box.w - 40, 260}, static_cast<int>(current.downloads.size()), 48,
                        [&](int index, const Rect& row) {
                            const auto& download = current.downloads[current.downloads.size() - 1 - static_cast<std::size_t>(index)];
                            label(ui, download.name, {row.x, row.y, row.w, 23}, 13);
                            label(ui, download.finished ? (download.success ? "本机发送完成，保存状态由对方确认" : "下载中断，对方可以重试") :
                                  "正在发送 " + readable_size(download.transferred) + " / " + readable_size(download.size),
                                  {row.x, row.y + 23, row.w, 21}, 11, muted);
                        });
                    if (button(ui, "关闭", {box.right() - 110, box.bottom() - 45, 90, 30})) view.downloads_open = false;
                    if (!view.failed_files.empty()) {
                        if (button(ui, "失败详情", {box.x + 20, box.bottom() - 45, 105, 30})) {
                            view.message_detail = tempshare::Message{view.file_failures, true}; view.detail_text = view.file_failures;
                        }
                        if (button(ui, "重试失败项", {box.x + 136, box.bottom() - 45, 115, 30})) {
                            const auto files = view.failed_files; share_paths(session, view, files);
                        }
                    }
                }
            }
            if (requests_open) {
                const Rect box{(width - 420) / 2, (height - 330) / 2, 420, 330};
                if (ui.dialog("所有设备的待处理请求", "选择设备查看配对或文件请求。", &requests_open, box)) {
                    float request_y = box.y + 91;
                    for (std::size_t slot = 0; slot < requests.size(); ++slot) {
                        if (requests[slot].empty()) continue;
                        if (button(ui, requests[slot], {box.x + 20, request_y, box.w - 40, 35}, false, 9100 + slot)) {
                            selected_connection = static_cast<int>(slot);
                            connections[slot]->view.connection_open = true;
                            connections[slot]->view.page = View::Page::share;
                            requests_open = false;
                        }
                        request_y += 42;
                    }
                    if (!request_count) label(ui, "当前没有待处理请求", {box.x + 20, request_y, box.w - 40, 28}, 14, muted);
                }
            }
            if (view.confirm_clear) {
                const Rect box{(width - 400) / 2, (height - 210) / 2, 400, 210};
                if (ui.alert_dialog("清空双方的消息？", "此操作删除当前会话双方的同步记录，已保存文件保留。", "清空双方记录", "取消",
                                    &view.confirm_clear, box, Style().destructive())) session.clear_messages();
            }
            if (view.permissions_pending) {
                bool open = true;
                const Rect box{(width - 400) / 2, (height - 210) / 2, 400, 210};
                if (ui.alert_dialog("停止相关任务并撤销权限？", "关闭文件接收或屏幕权限会停止正在进行的相关任务。已有消息和已保存文件保留。", "撤销权限", "保留权限",
                                    &open, box, Style().destructive())) {
                    session.set_permissions(*view.permissions_pending); view.permissions_pending.reset();
                } else if (!open) view.permissions_pending.reset();
            }
            if (view.confirm_leave) {
                const Rect box{(width - 400) / 2, (height - 210) / 2, 400, 210};
                if (ui.alert_dialog("离开此设备？", "将停止本设备的投屏与未完成传输，共享清单和消息仍保留在本机会话。", "离开设备", "继续连接",
                                    &view.confirm_leave, box)) {
                    if (snapshot.pairing) session.disconnect(snapshot.pairing->token);
                    http.peer().leave();
                }
            }
            if (view.message_detail) {
                bool open = true;
                const Rect box{(width - 540) / 2, (height - 420) / 2, 540, 420};
                ui.dialog("消息详情", "", &open, box);
                ui.textarea(&view.detail_text, {box.x + 20, box.y + 56, box.w - 40, 270}, Style().font(14));
                if (button(ui, "复制原文", {box.x + 20, box.bottom() - 56, 105, 32}))
                    view.status = window.try_set_clipboard(view.message_detail->text) ? "已复制" : "复制失败";
                const auto& value = view.message_detail->text;
                if ((value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0) && value.find_first_of(" \r\n\t") == std::string::npos &&
                    button(ui, "打开链接", {box.x + 136, box.bottom() - 56, 105, 32})) open_url(value);
                if (button(ui, "关闭", {box.right() - 110, box.bottom() - 56, 90, 32}) || !open) view.message_detail.reset();
            }
            if (close_dialog_open) {
                bool can_tray = false;
#ifdef _WIN32
                can_tray = desktop.tray;
#endif
                const float dialog_width = std::min(390.0f, width - 40.0f);
                const Rect dialog{(width - dialog_width) / 2.0f, (height - 194.0f) / 2.0f,
                                  dialog_width, 194.0f};
                bool open = true;
                ui.dialog("关闭 TempShare",
                          can_tray ? "缩小到托盘后，连接和传输会继续运行。直接退出会结束当前会话。"
                                       : "直接退出会结束当前会话。",
                          &open, dialog);
                const float button_y = dialog.bottom() - 46.0f;
                const float gap = 8.0f;
                const float button_width = can_tray ? (dialog.w - 48.0f - gap * 2.0f) / 3.0f
                                                        : (dialog.w - 48.0f - gap) / 2.0f;
                float button_x = dialog.x + 24.0f;
#ifdef _WIN32
                if (can_tray && button(ui, "缩小到托盘", {button_x, button_y, button_width, 30})) {
                    desktop.minimize();
                    close_dialog_open = false;
                }
#endif
                if (can_tray) button_x += button_width + gap;
                if (button(ui, "直接退出", {button_x, button_y, button_width, 30}, true)) {
                    close_dialog_open = false;
                    window.close();
                }
                button_x += button_width + gap;
                if (button(ui, "继续使用", {button_x, button_y, button_width, 30})) {
                    close_dialog_open = false;
                }
                if (!open) {
                    close_dialog_open = false;
                }
            }
        });
    } catch (...) {
        stop_timer();
        primary->http.stop();
        for (const auto& connection : connections) if (connection) connection->http.stop();
        for (const auto& connection : connections) if (connection) connection->session.end();
        throw;
    }
    stop_timer();
    primary->http.stop();
    for (const auto& connection : connections) if (connection) connection->http.stop();
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
