#include "core/session.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <utility>

namespace tempshare {
namespace {

constexpr auto cast_timeout = std::chrono::seconds(8);

std::string short_code(const std::string& previous = {}) {
    std::random_device random;
    std::uniform_int_distribution<int> digits(100000, 999999);
    std::string code;
    do {
        code = std::to_string(digits(random));
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
    std::random_device random;
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::string result;
    for (int i = 0; i < 16; ++i) {
        const auto value = byte(random);
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

Session::Session(std::filesystem::path directory, std::function<void()> changed, Clock::duration lifetime,
                 std::filesystem::path trust_directory)
    : changed_(std::move(changed)), lifetime_(lifetime), directory_(std::move(directory)),
      trust_directory_(std::move(trust_directory)) {
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

bool Session::active() const { return Clock::now() < expires_; }

bool Session::has_access(std::string_view token) const {
    return active() && pairing_ && pairing_->state == State::approved && equals(token, pairing_->token);
}

void Session::notify() const {
    if (changed_) changed_();
}

void Session::start() {
    std::lock_guard lock(mutex_);
    invitation_ = random_token();
    code_ = short_code(code_);
    expires_ = Clock::now() + lifetime_;
    retry_after_ = {};
    failed_codes_ = 0;
    permissions_ = {};
    reset_content();
    notify();
}

void Session::reset_content() {
    pairing_.reset();
    offers_.clear();
    partials_.clear();
    shared_.clear();
    staging_.clear();
    messages_.clear();
    ++text_revision_;
    cast_token_.clear();
    cast_ = {};
}

void Session::end() {
    std::lock_guard lock(mutex_);
    expires_ = {};
    reset_content();
    notify();
}

bool Session::expire() {
    std::lock_guard lock(mutex_);
    if (expires_ == Clock::time_point{} || active()) return false;
    expires_ = {};
    reset_content();
    notify();
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

Snapshot Session::snapshot() const {
    std::lock_guard lock(mutex_);
    if (!active()) return {false, {}, {}, {}, {}, {}, received_, directory_, {}, {}, false,
                           !trust_directory_.empty(), trusted_};
    return {true, invitation_, code_, pairing_, offers_, shared_, received_, directory_,
            permissions_, messages_, !cast_token_.empty(), !trust_directory_.empty(), trusted_};
}

bool Session::set_directory(std::filesystem::path directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return false;
    std::lock_guard lock(mutex_);
    directory_ = std::move(directory);
    staging_.clear();
    notify();
    return true;
}

bool Session::share(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const auto size = std::filesystem::file_size(path, error);
    const std::string name = path.filename().u8string();
    if (error || size > max_file_size || !utf8(name)) return false;
    std::lock_guard lock(mutex_);
    if (!active() || shared_.size() >= 100) return false;
    shared_.push_back({random_token(), name, size, path});
    notify();
    return true;
}

bool Session::share_archive(std::string_view invitation, std::string name,
                            std::shared_ptr<const std::filesystem::path> directory) {
    if (!directory || !valid_filename(name)) return false;
    const auto path = *directory / "folder.zip";
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > max_file_size) return false;
    std::lock_guard lock(mutex_);
    if (!active() || !equals(invitation, invitation_) || shared_.size() >= 100) return false;
    shared_.push_back({random_token(), std::move(name), size, path, std::move(directory)});
    notify();
    return true;
}

void Session::unshare(const std::string& id) {
    std::lock_guard lock(mutex_);
    shared_.erase(std::remove_if(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    }), shared_.end());
    notify();
}

std::optional<Pairing> Session::pair(std::string_view invitation, std::string name) {
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
    notify();
    return pairing_;
}

std::optional<Pairing> Session::reconnect(std::string_view trusted) {
    std::lock_guard lock(mutex_);
    if (!active() || pairing_ || trust_directory_.empty()) return {};
    const auto device = std::find_if(trusted_.begin(), trusted_.end(), [trusted](const TrustedDevice& entry) {
        return equals(trusted, entry.key);
    });
    if (device == trusted_.end()) return {};
    permissions_ = device->permissions;
    pairing_ = Pairing{random_token(), {}, State::approved, device->name, device->key};
    notify();
    return pairing_;
}

bool Session::remember_device() {
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
        notify();
        return true;
    }
    if (trusted_.size() >= 24) return false;
    auto key = random_token();
    TrustedDevice device{key, pairing_->name, permissions_};
    trusted_.reserve(trusted_.size() + 1);
    if (!save_trusted_device(trust_directory_, device)) return false;
    trusted_.push_back(std::move(device));
    pairing_->trusted_key = std::move(key);
    notify();
    return true;
}

bool Session::revoke_device(const std::string& key) {
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
    notify();
    return true;
}

std::optional<Pairing> Session::pairing(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || !equals(token, pairing_->token)) return {};
    return pairing_;
}

bool Session::decide_pairing(const std::string& token, bool accept) {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->token != token || pairing_->state != State::pending) return false;
    if (accept) {
        pairing_->state = State::approved;
    } else {
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
    }
    notify();
    return true;
}

bool Session::disconnect(std::string_view token) {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || !equals(token, pairing_->token)) return false;
    if (pairing_->state == State::pending) {
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
    } else {
        expires_ = {};
        reset_content();
    }
    notify();
    return true;
}

bool Session::authorized(std::string_view token) const {
    std::lock_guard lock(mutex_);
    return has_access(token);
}

bool Session::downloading(std::string_view token, const std::string& id) const {
    std::lock_guard lock(mutex_);
    return has_access(token) && permissions_.download && std::any_of(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    });
}

std::optional<std::vector<File>> Session::shared(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    if (!permissions_.download) return std::vector<File>{};
    return shared_;
}

std::optional<Offer> Session::offer(std::string_view token, std::string name, std::uint64_t size) {
    if (!valid_filename(name) || size > max_file_size) return {};
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return {};
    if (offers_.size() >= 100) return {};
    offers_.push_back({random_token(), std::move(name), size});
    notify();
    return offers_.back();
}

std::optional<std::vector<Offer>> Session::offers(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    return offers_;
}

bool Session::decide_offer(const std::string& id, bool accept) {
    std::lock_guard lock(mutex_);
    if (!active()) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::pending) return false;
    offer->state = accept ? State::approved : State::rejected;
    notify();
    return true;
}

bool Session::cancel_offer(std::string_view token, const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state == State::complete) return false;
    offer->state = State::rejected;
    partials_.erase(id);
    notify();
    return true;
}

std::optional<Upload> Session::begin_upload(std::string_view token, const std::string& id,
                                           std::uint64_t size, std::optional<std::uint64_t> resume_from) {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return {};
    auto offer = find_offer(offers_, id);
    const auto offset = resume_from.value_or(0);
    if (offer == offers_.end() || offer->state != State::approved || offer->transferred != offset ||
        size > offer->size - offset || (!size && offer->size != offset) ||
        (!resume_from && size != offer->size)) return {};
    if (const auto found = partials_.find(id); found != partials_.end()) {
        offer->state = State::uploading;
        notify();
        return found->second;
    }
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) return {};
    if (staging_.empty()) {
        const auto folder = directory_ / ("TempShare-" + random_token().substr(0, 12));
        if (!std::filesystem::create_directory(folder, error)) return {};
        staging_ = folder;
    }
    const auto name = std::filesystem::u8path(offer->name);
    const auto destination = staging_ / name;
    const auto temporary = std::shared_ptr<const std::filesystem::path>(
        new std::filesystem::path(staging_ / (".upload-" + random_token() + ".part")),
        [](const std::filesystem::path* path) {
            std::error_code ignored;
            std::filesystem::remove(*path, ignored);
            delete path;
        });
    const Upload upload{temporary, destination};
    partials_.emplace(id, upload);
    offer->state = State::uploading;
    notify();
    return upload;
}

bool Session::finish_upload(std::string_view token, const std::string& id, const Upload& upload,
                            std::optional<std::uint64_t> transferred) {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::uploading) return false;
    if (transferred && (*transferred < offer->transferred || *transferred > offer->size)) return false;
    if (transferred && *transferred < offer->size) {
        offer->transferred = *transferred;
        offer->state = State::approved;
        notify();
        return true;
    }
    std::error_code error;
    auto destination = upload.destination;
    for (unsigned suffix = 2; std::filesystem::exists(destination, error); ++suffix) {
        const auto prefix = std::to_string(suffix) + "-";
        const auto name = upload.destination.filename().u8string();
        destination = upload.destination.parent_path() / std::filesystem::u8path(prefix + name);
    }
    if (error) return false;
    File received{id, destination.filename().u8string(), offer->size, destination};
    received_.reserve(received_.size() + 1);
    std::filesystem::rename(*upload.temporary, destination, error);
    if (error) return false;
    offer->state = State::complete;
    offer->transferred = offer->size;
    partials_.erase(id);
    received_.push_back(std::move(received));
    notify();
    return true;
}

bool Session::uploading(std::string_view token, const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.upload) return false;
    const auto offer = find_offer(offers_, id);
    return offer != offers_.end() && offer->state == State::uploading;
}

void Session::retry_upload(std::string_view token, const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return;
    auto offer = find_offer(offers_, id);
    if (offer != offers_.end() && offer->state == State::uploading) {
        offer->state = State::approved;
        notify();
    }
}

void Session::set_permissions(Permissions permissions) {
    std::lock_guard lock(mutex_);
    if (!active()) return;
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
    notify();
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
    messages_.push_back({std::move(text), from_desktop});
    ++text_revision_;
    notify();
    return true;
}

bool Session::send_text(std::string text) {
    std::lock_guard lock(mutex_);
    return active() && pairing_ && pairing_->state == State::approved && add_message(std::move(text), true);
}

bool Session::send_text(std::string_view token, std::string text) {
    std::lock_guard lock(mutex_);
    return has_access(token) && add_message(std::move(text), false);
}

std::optional<std::vector<Message>> Session::messages(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !permissions_.text) return {};
    return messages_;
}

std::uint64_t Session::text_revision() const {
    std::lock_guard lock(mutex_);
    return text_revision_;
}

void Session::clear_messages() {
    std::lock_guard lock(mutex_);
    messages_.clear();
    ++text_revision_;
    notify();
}

bool Session::cast_available() const {
    return active() && !cast_token_.empty() && Clock::now() - cast_host_seen_ < cast_timeout;
}

bool Session::cast_host_access(std::string_view token) const {
    return active() && !cast_token_.empty() && equals(token, cast_token_);
}

std::string Session::open_cast() {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->state != State::approved) return {};
    cast_token_ = random_token();
    cast_ = {};
    cast_host_seen_ = Clock::now();
    cast_viewer_seen_ = {};
    notify();
    return cast_token_;
}

bool Session::stop_cast(std::string_view token) {
    std::lock_guard lock(mutex_);
    if (!token.empty() && !cast_host_access(token)) return false;
    cast_token_.clear();
    cast_ = {};
    notify();
    return true;
}

std::optional<CastSignal> Session::cast_host(std::string_view token) {
    std::lock_guard lock(mutex_);
    if (!cast_host_access(token)) return {};
    if (!cast_available()) {
        cast_ = {};
    }
    cast_host_seen_ = Clock::now();
    if (cast_.viewing && Clock::now() - cast_viewer_seen_ >= cast_timeout) cast_ = {};
    return cast_;
}

std::optional<CastSignal> Session::cast_viewer(std::string_view token, bool viewing) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    if (!cast_available()) return CastSignal{};
    if (viewing) {
        cast_viewer_seen_ = Clock::now();
        cast_.viewing = true;
    } else if (cast_.viewing) {
        cast_ = {};
        notify();
    }
    return cast_;
}

bool Session::cast_offer(std::string_view token, std::string sdp) {
    std::lock_guard lock(mutex_);
    if (!cast_available() || !cast_host_access(token) || !cast_.viewing) return false;
    cast_.offer = std::move(sdp);
    cast_.answer.clear();
    cast_.revision = ++cast_revision_;
    notify();
    return true;
}

bool Session::cast_answer(std::string_view token, unsigned revision, std::string sdp) {
    std::lock_guard lock(mutex_);
    if (!has_access(token) || !cast_available() || cast_.offer.empty() || !cast_.viewing ||
        cast_.revision != revision || !cast_.answer.empty()) return false;
    cast_.answer = std::move(sdp);
    return true;
}

}  // namespace tempshare
