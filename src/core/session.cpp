#include "core/session.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace tempshare {
namespace {

constexpr auto cast_timeout = std::chrono::seconds(8);
constexpr std::string_view upload_workspace = ".tempshare-partials";

bool is_upload_workspace(std::string_view name) {
    return name.size() == upload_workspace.size() &&
        std::equal(name.begin(), name.end(), upload_workspace.begin(),
                   [](unsigned char left, unsigned char right) { return std::tolower(left) == right; });
}

bool valid_upload_directory(std::string_view directory) {
    if (directory.size() > 1024 || (!directory.empty() && directory.back() == '/')) return false;
    unsigned depth = 0;
    while (!directory.empty()) {
        const auto slash = directory.find('/');
        const auto part = directory.substr(0, slash);
        if (++depth > 16 || !valid_filename(part) || is_upload_workspace(part)) return false;
        if (slash == std::string_view::npos) return true;
        directory.remove_prefix(slash + 1);
    }
    return true;
}

std::uint64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Notification {
    const std::function<void()>& callback;
    bool changed = false;

    ~Notification() noexcept {
        if (changed && callback) {
            try { callback(); } catch (...) { /* State has already committed. */ }
        }
    }
};

void secure_random(void* output, std::size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(output), static_cast<ULONG>(size),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("系统随机数不可用");
#elif defined(__APPLE__)
    arc4random_buf(output, size);
#else
    auto* bytes = static_cast<unsigned char*>(output);
    while (size) {
        const auto result = getrandom(bytes, size, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw std::runtime_error("系统随机数不可用");
        bytes += result;
        size -= static_cast<std::size_t>(result);
    }
#endif
}

std::string short_code(const std::string& previous = {}) {
    constexpr std::uint32_t count = 900000;
    constexpr std::uint32_t limit = UINT32_MAX / count * count;
    std::string code;
    do {
        std::uint32_t random = 0;
        do { secure_random(&random, sizeof(random)); } while (random >= limit);
        code = std::to_string(100000 + random % count);
    } while (code == previous);
    return code;
}

constexpr char hex[] = "0123456789abcdef";

bool equals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

bool utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        const int trailing = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
        if (!trailing || first > 0xF4 || i + trailing > value.size()) return false;
        const auto second = static_cast<unsigned char>(value[i]);
        if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second > 0x9F) ||
            (first == 0xF0 && second < 0x90) || (first == 0xF4 && second > 0x8F)) return false;
        for (int j = 0; j < trailing; ++j) {
            if ((static_cast<unsigned char>(value[i++]) & 0xC0) != 0x80) return false;
        }
    }
    return true;
}

template<typename Offers>
auto find_offer(Offers& offers, const std::string& id) {
    return std::find_if(offers.begin(), offers.end(), [&](const Offer& offer) {
        return offer.id == id;
    });
}

bool save_trusted_device(const std::filesystem::path& directory, const TrustedDevice& device) {
    const auto target = directory / device.key;
    auto pending = target;
    pending += ".new";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    const auto& permissions = device.permissions;
    output << permissions.download << permissions.upload << permissions.text << '\n' << device.name << '\n';
    output.close();
    std::error_code error;
    if (output) std::filesystem::rename(pending, target, error);
    if (output && !error) return true;
    std::filesystem::remove(pending, error);
    return false;
}

}  // namespace

std::string random_token() {
    std::array<unsigned char, 16> bytes{};
    secure_random(bytes.data(), bytes.size());
    std::string result;
    result.reserve(32);
    for (const auto value : bytes) {
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}

std::string json_string(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

std::string_view state_name(State state) {
    switch (state) {
    case State::pending: return "pending";
    case State::approved: return "approved";
    case State::rejected: return "rejected";
    case State::uploading: return "uploading";
    case State::complete: return "complete";
    }
    return {};
}

bool valid_filename(std::string_view name) {
    if (name.empty() || name.size() > 240 || name.back() == '.' || name.back() == ' ' || !utf8(name)) {
        return false;
    }
    for (const unsigned char c : name) {
        if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            return false;
        }
    }
    auto stem = std::string(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::toupper(c); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) {
        const auto suffix = stem.substr(3);
        if ((suffix.size() == 1 && suffix.front() >= '0' && suffix.front() <= '9') ||
            suffix == "¹" || suffix == "²" || suffix == "³") return false;
    }
    return true;
}

void clean_interrupted_uploads(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(48);
    const auto matches = [](std::string_view name, std::string_view prefix,
                            std::size_t length, std::string_view suffix = {}) {
        return name.size() == prefix.size() + length + suffix.size() &&
            name.substr(0, prefix.size()) == prefix && name.substr(prefix.size() + length) == suffix &&
            name.substr(prefix.size(), length).find_first_not_of("0123456789abcdef") == std::string_view::npos;
    };
    std::error_code directory_error;
    const auto inbox = fs::canonical(directory, directory_error);
    if (directory_error) return;
    for (fs::directory_iterator folder(inbox, directory_error), end; !directory_error && folder != end;
         folder.increment(directory_error)) {
        std::error_code error;
        if (!matches(folder->path().filename().u8string(), "TempShare-", 12) ||
            !fs::is_directory(folder->symlink_status(error)) || error) continue;
        const auto resolved = fs::canonical(folder->path(), error);
        if (error || resolved != folder->path()) continue;
        const auto workspace = folder->path() / upload_workspace;
        if (!fs::is_directory(fs::symlink_status(workspace, error)) || error ||
            fs::canonical(workspace, error) != workspace || error) continue;
        for (fs::directory_iterator file(workspace, error); !error && file != end; file.increment(error)) {
            std::error_code file_error;
            if (!matches(file->path().filename().u8string(), ".upload-", 32, ".part") ||
                !fs::is_regular_file(file->symlink_status(file_error)) || file_error) continue;
            const auto modified = file->last_write_time(file_error);
            if (!file_error && modified < cutoff) fs::remove(file->path(), file_error);
        }
    }
}

Session::Session(std::filesystem::path directory, std::function<void()> changed, Clock::duration lifetime,
                 std::filesystem::path trust_directory, std::filesystem::path history_file)
    : changed_(std::move(changed)), lifetime_(lifetime), history_file_(std::move(history_file)), directory_(std::move(directory)),
      trust_directory_(std::move(trust_directory)) {
    if (!history_file_.empty()) {
        std::error_code error;
        const auto size = std::filesystem::file_size(history_file_, error);
        if (!error && size <= 2 * 1024 * 1024) {
            std::ifstream input(history_file_, std::ios::binary);
            File file;
            std::string path;
            while (received_.size() < 200 && input >> std::quoted(file.id) >> std::quoted(file.name) >> file.size >>
                   std::quoted(path) >> std::quoted(file.received_from) >> file.received_at) {
                if (!utf8(path) || !utf8(file.name) || !utf8(file.received_from)) continue;
                file.path = std::filesystem::u8path(path);
                if (file.path.is_absolute()) received_.push_back(file);
            }
        }
    }
    if (trust_directory_.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(trust_directory_, error);
#ifndef _WIN32
    if (!error) std::filesystem::permissions(trust_directory_, std::filesystem::perms::owner_all, error);
#endif
    if (error) {
        trust_directory_.clear();
        return;
    }
    auto entry = std::filesystem::directory_iterator(trust_directory_, error);
    if (error) {
        trust_directory_.clear();
        return;
    }
    for (; !error && entry != std::filesystem::directory_iterator() && trusted_.size() < 24; entry.increment(error)) {
        std::error_code entry_error;
        const auto key = entry->path().filename().u8string();
        if (key.size() != 32 || key.find_first_not_of(hex) != std::string::npos ||
            !std::filesystem::is_regular_file(entry->symlink_status(entry_error)) || entry_error ||
            entry->file_size(entry_error) > 256 || entry_error) continue;
        std::ifstream input(entry->path(), std::ios::binary);
        std::string flags;
        std::string name;
        if (std::getline(input, flags) && std::getline(input, name) && flags.size() == 3 &&
            flags.find_first_not_of("01") == std::string::npos && valid_filename(name)) {
            trusted_.push_back({key, std::move(name), {flags[0] == '1', flags[1] == '1', flags[2] == '1'}});
        }
    }
}

bool Session::save_received() const {
    if (history_file_.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(history_file_.parent_path(), error);
    if (error) return false;
    auto temporary = history_file_;
    temporary += "." + random_token() + ".tmp";
    std::ofstream output(temporary, std::ios::binary);
    for (const auto& file : received_) {
        output << std::quoted(file.id) << ' ' << std::quoted(file.name) << ' ' << file.size << ' ' <<
            std::quoted(file.path.u8string()) << ' ' << std::quoted(file.received_from) << ' ' << file.received_at << '\n';
    }
    output.close();
    if (output) std::filesystem::rename(temporary, history_file_, error);
    if (output && !error) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

bool Session::clear_received() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    auto previous = std::move(received_);
    if (!save_received()) {
        received_ = std::move(previous);
        return false;
    }
    notification.changed = true;
    return true;
}

bool Session::active() const { return Clock::now() < expires_; }

bool Session::has_access(std::string_view token) const {
    return active() && pairing_ && pairing_->state == State::approved && equals(token, pairing_->token);
}

void Session::start() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    invitation_ = random_token();
    code_ = short_code(code_);
    expires_ = Clock::now() + lifetime_;
    retry_after_ = {};
    failed_codes_ = 0;
    permissions_ = {};
    if (pairing_) previous_pairing_error_ = "session_ended";
    reset_content();
    notification.changed = true;
}

void Session::reset_content() {
    if (pairing_) previous_pairing_.swap(pairing_->token);
    pairing_.reset();
    offers_.clear();
    ++offers_revision_;
    committed_bytes_ = 0;
    partials_.clear();
    shared_.clear();
    folders_.clear();
    ++files_revision_;
    staging_.clear();
    messages_.clear();
    ++text_revision_;
    cast_token_.clear();
    reset_cast_signal();
}

void Session::end() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    expires_ = {};
    previous_pairing_error_ = "session_ended";
    reset_content();
    notification.changed = true;
}

bool Session::extend() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active()) return false;
    expires_ = Clock::now() + lifetime_;
    notification.changed = true;
    return true;
}

bool Session::expire() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (active() && pairing_ && pairing_->state == State::pending && Clock::now() >= pairing_deadline_) {
        previous_pairing_ = std::move(pairing_->token);
        previous_pairing_error_ = "approval_expired";
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
        notification.changed = true;
        return true;
    }
    if (expires_ == Clock::time_point{} || active()) return false;
    expires_ = {};
    previous_pairing_error_ = "session_expired";
    reset_content();
    notification.changed = true;
    return true;
}

bool Session::is_active() const {
    std::lock_guard lock(mutex_);
    return active();
}

bool Session::invitation_active(std::string_view invitation) const {
    std::lock_guard lock(mutex_);
    return active() && equals(invitation, invitation_);
}

Snapshot Session::snapshot(SnapshotPage page) const {
    std::lock_guard lock(mutex_);
    Snapshot result;
    result.active = active();
    result.directory = directory_;
    result.trust_enabled = !trust_directory_.empty();
    if (page == SnapshotPage::all || page == SnapshotPage::received) result.received = received_;
    if (page == SnapshotPage::all || page == SnapshotPage::devices) result.trusted = trusted_;
    if (!result.active) return result;
    result.remaining_seconds = std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(expires_ - Clock::now()).count());
    result.invitation = invitation_;
    result.code = code_;
    result.pairing = pairing_;
    if (result.pairing && result.pairing->state == State::pending) {
        result.pairing->remaining_seconds = std::max<std::int64_t>(0,
            std::chrono::ceil<std::chrono::seconds>(pairing_deadline_ - Clock::now()).count());
    }
    if (page == SnapshotPage::all || page == SnapshotPage::share) {
        result.offers = offers_;
        result.shared = shared_;
    } else {
        for (const auto& offer : offers_) {
            if (offer.state == State::pending) result.offers.push_back(offer);
        }
    }
    if (page == SnapshotPage::all || page == SnapshotPage::sync) result.messages = messages_;
    result.permissions = permissions_;
    result.pending_transfers = static_cast<std::size_t>(std::count_if(offers_.begin(), offers_.end(), [](const Offer& item) {
        return item.state == State::pending || item.state == State::approved || item.state == State::uploading;
    }));
    result.casting = !cast_token_.empty();
    result.capture_ready = cast_available() && cast_.ready;
    result.viewer_answered = cast_available() && !cast_.answer.empty();
    result.cast_compatibility = result.capture_ready && cast_.frames;
    return result;
}

bool Session::set_directory(std::filesystem::path directory) {
    std::error_code error;
    directory = std::filesystem::canonical(directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) return false;
    const auto probe = directory / (".tempshare-write-" + random_token());
    if (!std::filesystem::create_directory(probe, error)) return false;
    if (!std::filesystem::remove(probe, error) || error) return false;
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    directory_ = std::move(directory);
    staging_.clear();
    notification.changed = true;
    return true;
}

NotificationState Session::notification_state() const {
    std::lock_guard lock(mutex_);
    NotificationState result{directory_, {}, {}, received_.empty() ? std::string{} : received_.back().id};
    if (!active()) return result;
    if (pairing_ && pairing_->state == State::pending) result.pairing = pairing_->token;
    const auto pending = std::find_if(offers_.rbegin(), offers_.rend(), [](const Offer& offer) {
        return offer.state == State::pending;
    });
    if (pending != offers_.rend()) result.offer = pending->id;
    return result;
}

bool Session::share(const std::filesystem::path& path, bool image) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path, error);
    if (error) return false;
    const auto source = SharedSource(resolved).version();
    const std::string name = path.filename().u8string();
    if (!source || source->size > max_file_size || !utf8(name)) return false;
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || shared_.size() >= 100) return false;
    const auto id = random_token();
    shared_.push_back({id, name, source->size, resolved, source});
    if (image && add_message(name, true)) {
        messages_.back().attachment = id;
    }
    ++files_revision_;
    notification.changed = true;
    return true;
}

bool Session::share_folder(std::string_view invitation, const std::filesystem::path& path,
                           const std::string& refresh, const std::function<bool()>& cancelled,
                           const std::function<void(FolderProgress)>& progress) {
    namespace fs = std::filesystem;
    if (!invitation_active(invitation)) return false;
    const auto root = fs::canonical(path);
    auto name = root.filename().u8string();
    if (name.empty() || !utf8(name) || is_upload_workspace(name) || !fs::is_directory(root)) return false;
    constexpr std::size_t folder_limit = 10000;
    constexpr std::size_t session_folder_limit = 30000;
    const auto id = refresh.empty() ? random_token() : refresh;
    std::unordered_map<fs::path, File> previous;
    if (!refresh.empty()) {
        std::lock_guard lock(mutex_);
        const auto found = folders_.find(refresh);
        if (found == folders_.end() || found->second.at(refresh).entry.path != root) return false;
        for (const auto& [entry_id, node] : found->second) previous.emplace(node.entry.path, node.entry);
    }
    File shared{id, std::move(name), 0, root, {}, {}, true};
    FolderTree tree;
    tree.emplace(id, FolderNode{shared, {}});
    std::vector<std::string> parents{id};
    std::size_t scanned = 0;
    std::size_t retained = refresh.empty() ? 0 : 1;
    for (auto it = fs::recursive_directory_iterator(root), end = fs::recursive_directory_iterator(); it != end; ++it) {
        if ((cancelled && cancelled()) || !invitation_active(invitation)) return false;
        if (++scanned > folder_limit) throw std::runtime_error("单个文件夹最多可分享 10000 个条目");
        if (progress && scanned % 20 == 0) progress({scanned - 1, scanned - tree.size()});
        const auto& entry = *it;
        const auto state = entry.symlink_status();
        bool linked = fs::is_symlink(state);
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        linked = linked || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
#endif
        auto entry_name = entry.path().filename().u8string();
        if (linked || !utf8(entry_name) || is_upload_workspace(entry_name) ||
            (!fs::is_directory(state) && !fs::is_regular_file(state))) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::canonical(entry.path()) != entry.path()) {
            it.disable_recursion_pending();
            continue;
        }
        const auto directory = fs::is_directory(state);
        std::optional<SourceVersion> source;
        std::uint64_t size = 0;
        if (!directory) {
            source = SharedSource(entry.path()).version();
            if (fs::canonical(entry.path()) != entry.path()) continue;
            if (source) {
                size = source->size;
                if (size > max_file_size) source.reset();
            }
        }
        const auto old = previous.find(entry.path());
        if (old != previous.end()) ++retained;
        const auto entry_id = old != previous.end() && old->second.folder == directory &&
            (directory || (source && old->second.source == source)) ? old->second.id : random_token();
        const auto depth = static_cast<std::size_t>(it.depth());
        tree.at(parents[depth]).children.push_back(entry_id);
        tree.emplace(entry_id, FolderNode{{entry_id, std::move(entry_name), size, entry.path(), source, {}, directory}, {}});
        parents.resize(depth + 1);
        if (directory) parents.push_back(entry_id);
    }
    for (auto& [entry_id, node] : tree) {
        std::sort(node.children.begin(), node.children.end(), [&](const std::string& left, const std::string& right) {
            const auto& a = tree.at(left).entry;
            const auto& b = tree.at(right).entry;
            return a.folder == b.folder ? a.name < b.name : a.folder;
        });
    }
    if (progress) progress({scanned, scanned + 1 - tree.size(), tree.size() - retained - (refresh.empty() ? 1 : 0),
                            previous.size() - retained});
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if ((cancelled && cancelled()) || !active() || !equals(invitation, invitation_)) return false;
    std::size_t entries = tree.size() - 1;
    for (const auto& folder : folders_) if (folder.first != refresh) entries += folder.second.size() - 1;
    if (entries > session_folder_limit) throw std::runtime_error("本次会话最多可分享 30000 个文件夹条目");
    if (refresh.empty()) {
        if (shared_.size() >= 100) return false;
        shared_.push_back(std::move(shared));
        try {
            folders_.emplace(id, std::move(tree));
        } catch (...) {
            shared_.pop_back();
            throw;
        }
    } else {
        const auto found = folders_.find(refresh);
        if (found == folders_.end()) return false;
        found->second.swap(tree);
    }
    ++files_revision_;
    notification.changed = true;
    return true;
}

bool Session::share_archive(std::string_view invitation, std::string name,
                            std::shared_ptr<const std::filesystem::path> directory) {
    if (!directory || !valid_filename(name)) return false;
    const auto path = *directory / "folder.zip";
    const auto source = SharedSource(path).version();
    if (!source || source->size > max_file_size) return false;
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !equals(invitation, invitation_) || shared_.size() >= 100) return false;
    shared_.push_back({random_token(), std::move(name), source->size, path, source, std::move(directory)});
    ++files_revision_;
    notification.changed = true;
    return true;
}

void Session::unshare(const std::string& id) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    folders_.erase(id);
    shared_.erase(std::remove_if(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    }), shared_.end());
    ++files_revision_;
    notification.changed = true;
}

std::optional<Pairing> Session::pair(std::string_view invitation, std::string name) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || pairing_) return {};
    if (!equals(invitation, invitation_)) {
        const auto now = Clock::now();
        if (now < retry_after_) return {};
        if (!equals(invitation, code_)) {
            if (++failed_codes_ >= 10) {
                failed_codes_ = 0;
                retry_after_ = now + std::chrono::minutes(1);
            }
            return {};
        }
    }
    if (!valid_filename(name) || name.size() > 80) name = "浏览器";
    pairing_ = Pairing{random_token(), short_code(), State::pending, std::move(name), {}};
    pairing_deadline_ = Clock::now() + std::chrono::minutes(2);
    notification.changed = true;
    return pairing_;
}

std::optional<Pairing> Session::reconnect(std::string_view trusted) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || pairing_ || trust_directory_.empty()) return {};
    const auto device = std::find_if(trusted_.begin(), trusted_.end(), [trusted](const TrustedDevice& entry) {
        return equals(trusted, entry.key);
    });
    if (device == trusted_.end()) return {};
    permissions_ = device->permissions;
    pairing_ = Pairing{random_token(), {}, State::approved, device->name, device->key};
    notification.changed = true;
    return pairing_;
}

bool Session::remember_device() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->state != State::approved || trust_directory_.empty()) return false;
    if (!pairing_->trusted_key.empty()) {
        const auto existing = std::find_if(trusted_.begin(), trusted_.end(), [&](const TrustedDevice& item) {
            return item.key == pairing_->trusted_key;
        });
        if (existing == trusted_.end()) return false;
        const TrustedDevice updated{existing->key, existing->name, permissions_};
        if (!save_trusted_device(trust_directory_, updated)) return false;
        existing->permissions = permissions_;
        notification.changed = true;
        return true;
    }
    if (trusted_.size() >= 24) return false;
    auto key = random_token();
    TrustedDevice device{key, pairing_->name, permissions_};
    trusted_.reserve(trusted_.size() + 1);
    if (!save_trusted_device(trust_directory_, device)) return false;
    trusted_.push_back(std::move(device));
    pairing_->trusted_key = std::move(key);
    notification.changed = true;
    return true;
}

bool Session::revoke_device(const std::string& key) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    const auto device = std::find_if(trusted_.begin(), trusted_.end(), [&key](const TrustedDevice& entry) {
        return entry.key == key;
    });
    if (device == trusted_.end()) return false;
    std::error_code error;
    std::filesystem::remove(trust_directory_ / key, error);
    if (error) return false;
    trusted_.erase(device);
    if (pairing_ && pairing_->trusted_key == key) {
        reset_content();
        permissions_ = {};
        invitation_ = random_token();
        code_ = short_code(code_);
    }
    notification.changed = true;
    return true;
}

std::optional<Pairing> Session::pairing(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || !equals(token, pairing_->token)) return {};
    auto result = *pairing_;
    result.remaining_seconds = std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(expires_ - Clock::now()).count());
    if (result.state == State::pending) result.remaining_seconds = std::max<std::int64_t>(0,
        std::chrono::ceil<std::chrono::seconds>(pairing_deadline_ - Clock::now()).count());
    return result;
}

std::string_view Session::pairing_error(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (token.empty()) return "not_paired";
    if (equals(token, previous_pairing_)) return previous_pairing_error_;
    if (pairing_ && !active() && equals(token, pairing_->token)) return "session_expired";
    return "session_ended";
}

bool Session::decide_pairing(const std::string& token, bool accept) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->token != token || pairing_->state != State::pending ||
        Clock::now() >= pairing_deadline_) return false;
    if (accept) {
        pairing_->state = State::approved;
    } else {
        previous_pairing_ = std::move(pairing_->token);
        previous_pairing_error_ = "pairing_denied";
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
    }
    notification.changed = true;
    return true;
}

bool Session::disconnect(std::string_view token) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || !equals(token, pairing_->token)) return false;
    if (pairing_->state == State::pending) {
        previous_pairing_ = std::move(pairing_->token);
        previous_pairing_error_ = "pairing_cancelled";
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
    } else {
        expires_ = {};
        previous_pairing_error_ = "session_ended";
        reset_content();
    }
    notification.changed = true;
    return true;
}

bool Session::authorized(std::string_view token) const {
    std::lock_guard lock(mutex_);
    return has_access(token);
}

bool Session::upload_allowed(std::string_view token) const {
    std::lock_guard lock(mutex_);
    return has_access(token) && permissions_.upload;
}
bool Session::downloading(std::string_view token, const std::string& id, const std::string& folder) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.download) return false;
    if (!folder.empty()) {
        const auto root = folders_.find(folder);
        return root != folders_.end() && root->second.find(id) != root->second.end();
    }
    return std::any_of(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    });
}

std::optional<std::vector<File>> Session::shared(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    if (!permissions_.download) return std::vector<File>{};
    return shared_;
}

std::optional<std::vector<File>> Session::folder_contents(std::string_view token, const std::string& root,
                                                         const std::string& directory) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.download) return {};
    const auto folder = folders_.find(root);
    if (folder == folders_.end()) return {};
    const auto child = folder->second.find(directory);
    if (child == folder->second.end() || !child->second.entry.folder) return {};
    std::vector<File> entries;
    entries.reserve(child->second.children.size());
    for (const auto& id : child->second.children) entries.push_back(folder->second.at(id).entry);
    return entries;
}

std::optional<File> Session::folder_file(std::string_view token, const std::string& root, const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.download) return {};
    const auto folder = folders_.find(root);
    if (folder == folders_.end()) return {};
    const auto child = folder->second.find(id);
    if (child == folder->second.end() || child->second.entry.folder) return {};
    return child->second.entry;
}

std::optional<std::vector<File>> Session::folder_selection(std::string_view token, const std::string& root,
    const std::string& directory, const std::vector<std::string>& selected) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.download) return {};
    const auto folder = folders_.find(root);
    if (folder == folders_.end()) return {};
    const auto parent = folder->second.find(directory);
    if (parent == folder->second.end() || !parent->second.entry.folder) return {};
    std::vector<File> result{parent->second.entry};
    std::vector<std::string> pending = selected.empty() ? parent->second.children : selected;
    if (!selected.empty()) {
        std::sort(pending.begin(), pending.end());
        pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
        if (std::any_of(pending.begin(), pending.end(), [&](const auto& id) {
            const auto& children = parent->second.children;
            return std::find(children.begin(), children.end(), id) == children.end();
        })) return {};
    }
    while (!pending.empty()) {
        const auto& node = folder->second.at(pending.back());
        pending.pop_back();
        result.push_back(node.entry);
        pending.insert(pending.end(), node.children.begin(), node.children.end());
    }
    return result;
}

std::optional<Offer> Session::offer(std::string_view token, std::string name, std::uint64_t size, std::string batch,
                                    std::string relative_directory, bool image) {
    if (!valid_filename(name) || size > max_file_size || is_upload_workspace(name) ||
        !valid_upload_directory(relative_directory)) return {};
    if (!batch.empty() && (batch.size() != 32 || batch.find_first_not_of("0123456789abcdef") != std::string::npos)) return {};
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return {};
    const auto pending = std::count_if(offers_.begin(), offers_.end(), [](const Offer& offer) {
        return offer.state != State::complete && offer.state != State::rejected;
    });
    if (pending >= 100) return {};
    if (offers_.size() >= 200) {
        const auto terminal = std::find_if(offers_.begin(), offers_.end(), [](const Offer& offer) {
            return offer.state == State::complete || offer.state == State::rejected;
        });
        if (terminal != offers_.end()) offers_.erase(terminal);
    }
    offers_.push_back({random_token(), std::move(name), size, State::pending, 0, std::move(batch),
                       std::move(relative_directory), image});
    ++offers_revision_;
    notification.changed = true;
    return offers_.back();
}

std::optional<std::vector<Offer>> Session::offers(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    return offers_;
}

std::uint64_t Session::files_revision() const {
    std::lock_guard lock(mutex_);
    return files_revision_;
}

std::uint64_t Session::offers_revision() const {
    std::lock_guard lock(mutex_);
    return offers_revision_;
}

bool Session::decide_offer(const std::string& id, bool accept) {
    return decide_offers({id}, accept) != 0;
}

std::size_t Session::decide_offers(const std::vector<std::string>& ids, bool accept) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !permissions_.upload) return 0;
    std::size_t decided = 0;
    std::uint64_t reserved = committed_bytes_;
    for (const auto& offer : offers_) {
        if (offer.state == State::approved || offer.state == State::uploading) reserved += offer.size;
    }
    for (const auto& id : ids) {
        auto offer = find_offer(offers_, id);
        if (offer == offers_.end() || offer->state != State::pending) continue;
        if (accept && offer->size > max_session_bytes - reserved) continue;
        if (accept) reserved += offer->size;
        offer->state = accept ? State::approved : State::rejected;
        ++decided;
    }
    if (decided) {
        ++offers_revision_;
        notification.changed = true;
    }
    return decided;
}

bool Session::cancel_offer(std::string_view token, const std::string& id) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state == State::complete) return false;
    offer->state = State::rejected;
    ++offers_revision_;
    partials_.erase(id);
    notification.changed = true;
    return true;
}

std::optional<Upload> Session::begin_upload(std::string_view token, const std::string& id,
                                           std::uint64_t size, std::optional<std::uint64_t> resume_from,
                                           UploadFailure* failure) {
    Notification notification{changed_};
    std::unique_lock lock(mutex_);
    if (failure) *failure = UploadFailure::unavailable;
    if (!has_access(token) || !permissions_.upload) return {};
    const auto offset = resume_from.value_or(0);
    const auto matches = [&](const Offer& offer) {
        return offer.state == State::approved && offer.transferred == offset &&
            size <= offer.size - offset && (size || offer.size == offset) &&
            (resume_from || size == offer.size);
    };
    const auto pending = find_offer(offers_, id);
    if (pending == offers_.end() || !matches(*pending)) return {};
    const auto partial = partials_.find(id);
    const auto existing = partial == partials_.end() ? std::shared_ptr<const std::filesystem::path>{}
                                                     : partial->second.temporary;
    const auto target_directory = existing ? partial->second.destination.parent_path() : directory_;
    lock.unlock();
    std::error_code error;
    if (failure) *failure = UploadFailure::storage;
    if (!existing) std::filesystem::create_directories(target_directory, error);
    if (error) return {};
    const auto disk = std::filesystem::space(target_directory, error);
    if (error) return {};
    lock.lock();
    if (failure) *failure = UploadFailure::unavailable;
    if (!has_access(token) || !permissions_.upload) return {};
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || !matches(*offer)) return {};
    const auto found = partials_.find(id);
    const auto current = found == partials_.end() ? std::shared_ptr<const std::filesystem::path>{}
                                                  : found->second.temporary;
    if (current != existing || (!existing && directory_ != target_directory)) return {};
    std::uint64_t remaining = offer->size - offer->transferred;
    for (const auto& other : offers_) {
        if (other.id != id && other.state == State::uploading) remaining += other.size - other.transferred;
    }
    if (disk.available < minimum_free_bytes || remaining > disk.available - minimum_free_bytes) {
        if (failure) *failure = UploadFailure::insufficient_space;
        return {};
    }
    if (found != partials_.end()) {
        offer->state = State::uploading;
        ++offers_revision_;
        notification.changed = true;
        return found->second;
    }
    if (failure) *failure = UploadFailure::storage;
    if (staging_.empty()) {
        const auto root = std::filesystem::canonical(directory_, error);
        if (error) return {};
        const auto folder = root / ("TempShare-" + random_token().substr(0, 12));
        if (!std::filesystem::create_directory(folder, error)) return {};
        staging_ = folder;
    }
    const auto name = std::filesystem::u8path(offer->name);
    const auto stage = std::filesystem::canonical(staging_, error);
    if (error) return {};
    if (stage != staging_) return {};
    auto destination_directory = stage;
    for (const auto& part : std::filesystem::u8path(offer->relative_directory)) {
        destination_directory /= part;
        std::filesystem::create_directory(destination_directory, error);
        if (error || std::filesystem::canonical(destination_directory, error) != destination_directory || error) return {};
    }
    const auto destination = destination_directory / name;
    const auto workspace = staging_ / upload_workspace;
    std::filesystem::create_directory(workspace, error);
    if (error || !std::filesystem::is_directory(std::filesystem::symlink_status(workspace, error)) || error) return {};
    const auto temporary = std::shared_ptr<const std::filesystem::path>(
        new std::filesystem::path(workspace / (".upload-" + random_token() + ".part")),
        [](const std::filesystem::path* path) {
            std::error_code ignored;
            std::filesystem::remove(*path, ignored);
            delete path;
        });
    const Upload upload{temporary, destination};
    partials_.emplace(id, upload);
    offer->state = State::uploading;
    ++offers_revision_;
    notification.changed = true;
    return upload;
}

bool Session::finish_upload(std::string_view token, const std::string& id, const Upload& upload,
                            std::optional<std::uint64_t> transferred) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::uploading) return false;
    if (transferred && (*transferred < offer->transferred || *transferred > offer->size)) return false;
    if (transferred && *transferred < offer->size) {
        offer->transferred = *transferred;
        offer->state = State::approved;
        ++offers_revision_;
        notification.changed = true;
        return true;
    }
    std::error_code error;
    auto destination = upload.destination;
    if (std::filesystem::canonical(destination.parent_path(), error) != destination.parent_path() || error) return false;
    for (unsigned suffix = 2; std::filesystem::exists(destination, error); ++suffix) {
        const auto prefix = std::to_string(suffix) + "-";
        const auto name = upload.destination.filename().u8string();
        destination = upload.destination.parent_path() / std::filesystem::u8path(prefix + name);
    }
    if (error) return false;
    File received{id, destination.filename().u8string(), offer->size, destination};
    received.received_from = pairing_->name;
    received.received_at = unix_seconds();
    if (received_.size() < 200) received_.reserve(received_.size() + 1);
    std::filesystem::rename(*upload.temporary, destination, error);
    if (error) return false;
    if (offer->image) received.source = SharedSource(destination).version();
    offer->state = State::complete;
    committed_bytes_ += offer->size;
    ++offers_revision_;
    offer->transferred = offer->size;
    partials_.erase(id);
    if (received_.size() >= 200) received_.erase(received_.begin());
    received_.push_back(std::move(received));
    if (offer->image && add_message(received_.back().name, false)) {
        messages_.back().attachment = id;
    }
    try { save_received(); } catch (...) { /* The received file has already been committed. */ }
    notification.changed = true;
    return true;
}

bool Session::uploading(std::string_view token, const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return false;
    const auto offer = find_offer(offers_, id);
    return offer != offers_.end() && offer->state == State::uploading;
}

void Session::retry_upload(std::string_view token, const std::string& id) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return;
    auto offer = find_offer(offers_, id);
    if (offer != offers_.end() && offer->state == State::uploading) {
        offer->state = State::approved;
        ++offers_revision_;
        notification.changed = true;
    }
}

void Session::set_permissions(Permissions permissions) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active()) return;
    if (permissions_.download != permissions.download) ++files_revision_;
    if (permissions_.upload != permissions.upload) ++offers_revision_;
    permissions_ = permissions;
    if (!permissions_.upload) {
        for (auto& offer : offers_) {
            if (offer.state != State::complete) offer.state = State::rejected;
        }
        partials_.clear();
    }
    if (!permissions_.text && !messages_.empty()) {
        messages_.clear();
        ++text_revision_;
    }
    notification.changed = true;
}

Permissions Session::permissions() const {
    std::lock_guard lock(mutex_);
    return permissions_;
}

bool Session::add_message(std::string text, bool from_desktop) {
    if (!permissions_.text || text.size() > max_text_size || !utf8(text) ||
        text.find('\0') != std::string::npos || text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return false;
    }
    if (messages_.size() >= 50) messages_.erase(messages_.begin());
    messages_.push_back({std::move(text), from_desktop, unix_seconds()});
    ++text_revision_;
    return true;
}

bool Session::send_text(std::string text) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    notification.changed = active() && pairing_ && pairing_->state == State::approved && add_message(std::move(text), true);
    return notification.changed;
}

bool Session::send_text(std::string_view token, std::string text) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    notification.changed = has_access(token) && add_message(std::move(text), false);
    return notification.changed;
}

std::optional<std::vector<Message>> Session::messages(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.text) return {};
    return messages_;
}

std::optional<File> Session::message_attachment(std::string_view token, std::string_view id) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.text || !permissions_.download || id.empty()) return {};
    const auto message = std::find_if(messages_.begin(), messages_.end(), [&](const auto& item) {
        return item.attachment == id;
    });
    if (message == messages_.end()) return {};
    const auto& files = message->from_desktop ? shared_ : received_;
    const auto file = std::find_if(files.begin(), files.end(), [&](const auto& item) { return item.id == id; });
    return file == files.end() || !file->source ? std::nullopt : std::optional<File>(*file);
}

std::uint64_t Session::text_revision() const {
    std::lock_guard lock(mutex_);
    return text_revision_;
}

void Session::clear_messages() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    messages_.clear();
    ++text_revision_;
    notification.changed = true;
}

bool Session::clear_messages(std::string_view token) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.text) return false;
    messages_.clear();
    ++text_revision_;
    notification.changed = true;
    return true;
}

bool Session::cast_available() const {
    return active() && !cast_token_.empty() && Clock::now() - cast_host_seen_ < cast_timeout;
}

bool Session::cast_host_access(std::string_view token) const {
    return active() && !cast_token_.empty() && equals(token, cast_token_);
}

void Session::reset_cast_signal() {
    cast_ = {};
    cast_viewer_id_.clear();
    cast_frame_.jpeg.clear();
    cast_frame_at_ = {};
}

std::string Session::open_cast() {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->state != State::approved) return {};
    cast_token_ = random_token();
    reset_cast_signal();
    cast_host_seen_ = Clock::now();
    cast_viewer_seen_ = {};
    notification.changed = true;
    return cast_token_;
}

bool Session::stop_cast(std::string_view token) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!token.empty() && !cast_host_access(token)) return false;
    cast_token_.clear();
    reset_cast_signal();
    notification.changed = true;
    return true;
}

std::optional<CastSignal> Session::cast_host(std::string_view token) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!cast_host_access(token)) return {};
    if (!cast_available()) {
        reset_cast_signal();
    }
    cast_host_seen_ = Clock::now();
    if (cast_.viewing && Clock::now() - cast_viewer_seen_ >= cast_timeout) {
        const bool ready = cast_.ready;
        reset_cast_signal();
        cast_.ready = ready;
        notification.changed = true;
    }
    return cast_;
}

bool Session::cast_ready(std::string_view token) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!cast_host_access(token)) return false;
    cast_host_seen_ = Clock::now();
    if (!cast_.ready) {
        cast_.ready = true;
        notification.changed = true;
    }
    return true;
}

std::optional<bool> Session::capture_ready(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    return cast_available() && cast_.ready;
}

std::optional<CastSignal> Session::cast_viewer(std::string_view token, bool viewing, std::string_view viewer,
                                              bool frames) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    if (!viewer.empty() && (viewer.size() != 32 ||
        viewer.find_first_not_of("0123456789abcdef") != std::string_view::npos)) return {};
    if (!cast_available()) return CastSignal{};
    const auto now = Clock::now();
    if (cast_.viewing && cast_viewer_id_ != viewer) {
        if (!viewing || now - cast_viewer_seen_ < cast_timeout) return {};
        const bool ready = cast_.ready;
        reset_cast_signal();
        cast_.ready = ready;
    }
    if (viewing) {
        if (frames && !viewer.empty() && !cast_.frames && cast_.ready) {
            cast_.offer.clear();
            cast_.answer.clear();
            cast_.frames = true;
            cast_.revision = ++cast_revision_;
            notification.changed = true;
        }
        cast_viewer_id_.assign(viewer);
        cast_viewer_seen_ = now;
        cast_.viewing = true;
    } else if (cast_.viewing) {
        const bool ready = cast_.ready;
        reset_cast_signal();
        cast_.ready = ready;
        notification.changed = true;
    }
    return cast_;
}

bool Session::cast_offer(std::string_view token, std::string sdp) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!cast_available() || !cast_host_access(token) || !cast_.viewing || cast_.frames) return false;
    cast_.offer = std::move(sdp);
    cast_.ready = true;
    cast_.answer.clear();
    cast_.revision = ++cast_revision_;
    notification.changed = true;
    return true;
}

bool Session::cast_answer(std::string_view token, unsigned revision, std::string sdp, std::string_view viewer) {
    Notification notification{changed_};
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !cast_available() || cast_.offer.empty() || !cast_.viewing ||
        cast_.revision != revision || cast_viewer_id_ != viewer || !cast_.answer.empty()) return false;
    cast_.answer = std::move(sdp);
    notification.changed = true;
    return true;
}

bool Session::publish_cast_frame(std::string_view token, unsigned revision, std::string jpeg) {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    if (!cast_available() || !cast_host_access(token) || !cast_.frames || cast_.revision != revision ||
        now - cast_viewer_seen_ >= cast_timeout) return false;
    if (now - cast_frame_at_ < std::chrono::milliseconds(80)) return true;
    cast_frame_.jpeg = std::move(jpeg);
    ++cast_frame_.sequence;
    cast_frame_at_ = now;
    return true;
}

std::optional<CastFrame> Session::cast_frame(std::string_view token, std::string_view viewer, std::uint64_t after) const {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    if (!has_access(token) || !cast_available() || !cast_.frames || viewer != cast_viewer_id_ ||
        now - cast_viewer_seen_ >= cast_timeout) return {};
    if (cast_frame_.sequence <= after || now - cast_frame_at_ >= std::chrono::seconds(2)) return CastFrame{};
    return cast_frame_;
}

}  // namespace tempshare
